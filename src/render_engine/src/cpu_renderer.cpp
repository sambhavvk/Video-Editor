// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/edit_model/effect_evaluator.h"
#include "video_editor/render_engine/bitmap_glyphs.h"
#include "video_editor/render_engine/color_curves.h"
#include "video_editor/render_engine/lut3d.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::render {
namespace {

int scaled_dimension(const std::uint32_t value, const PreviewScale scale) {
  const int divisor = scale == PreviewScale::Full ? 1 : scale == PreviewScale::Half ? 2 : 4;
  return std::max(1, static_cast<int>(value) / divisor);
}

int preview_scale_divisor(const PreviewScale scale) {
  return scale == PreviewScale::Full ? 1 : scale == PreviewScale::Half ? 2 : 4;
}

struct TitleStyle final {
  std::string text;
  edit::ColorRgba foreground{1.0, 1.0, 1.0, 1.0};
  edit::ColorRgba background{0.0, 0.0, 0.0, 0.0};
  edit::TitleHorizontalAlignment alignment{edit::TitleHorizontalAlignment::Center};
  double font_size{96.0};
  bool bold{false};
  bool italic{false};
  int width{0};
  int height{0};
};

struct RenderedClip final {
  std::shared_ptr<CpuFrame> frame;
  edit::Clip clip;
};

[[nodiscard]] float saturate(const double value) noexcept {
  return std::clamp(static_cast<float>(value), 0.0F, 1.0F);
}

float blend_channel(const edit::BlendMode mode, const float source, const float destination) {
  switch (mode) {
  case edit::BlendMode::Add:
    return std::min(source + destination, 1.0F);
  case edit::BlendMode::Multiply:
    return source * destination;
  case edit::BlendMode::Screen:
    return 1.0F - ((1.0F - source) * (1.0F - destination));
  case edit::BlendMode::Overlay:
    return destination < 0.5F ? 2.0F * source * destination
                              : 1.0F - (2.0F * (1.0F - source) * (1.0F - destination));
  case edit::BlendMode::Normal:
  default:
    return source;
  }
}

edit::Time source_time_for(const edit::Clip& clip, const edit::Time timeline_time) {
  edit::Time offset = timeline_time - clip.timeline_range.start;
  offset = offset.scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
                         edit::RoundingMode::NearestTiesEven);
  return clip.reversed ? clip.source_range.end() - offset : clip.source_range.start + offset;
}

[[nodiscard]] double time_ratio(const edit::Time numerator, const edit::Time denominator) {
  if (denominator.isZero()) {
    return 0.0;
  }
  const long double left =
      static_cast<long double>(numerator.value()) / static_cast<long double>(numerator.timescale());
  const long double right = static_cast<long double>(denominator.value()) /
                            static_cast<long double>(denominator.timescale());
  return right == 0.0L ? 0.0 : static_cast<double>(left / right);
}

[[nodiscard]] bool inside_crop(const int x, const int y, const CpuFrame& source,
                               const edit::Transform& transform) noexcept {
  const double pixel_center_x = static_cast<double>(x) + 0.5;
  const double pixel_center_y = static_cast<double>(y) + 0.5;
  const double width = static_cast<double>(source.width());
  const double height = static_cast<double>(source.height());
  return pixel_center_x >= transform.crop_left * width &&
         pixel_center_x < (1.0 - transform.crop_right) * width &&
         pixel_center_y >= transform.crop_top * height &&
         pixel_center_y < (1.0 - transform.crop_bottom) * height;
}

[[nodiscard]] std::array<float, 4> sample_bilinear(const CpuFrame& source, const double x,
                                                   const double y,
                                                   const edit::Transform& transform) {
  constexpr double boundary_epsilon = 1.0e-9;
  const double maximum_x = static_cast<double>(source.width() - 1);
  const double maximum_y = static_cast<double>(source.height() - 1);
  if (x < -boundary_epsilon || y < -boundary_epsilon || x > maximum_x + boundary_epsilon ||
      y > maximum_y + boundary_epsilon) {
    return {};
  }
  const double bounded_x = std::clamp(x, 0.0, maximum_x);
  const double bounded_y = std::clamp(y, 0.0, maximum_y);

  const int x0 = static_cast<int>(std::floor(bounded_x));
  const int y0 = static_cast<int>(std::floor(bounded_y));
  const int x1 = std::min(x0 + 1, source.width() - 1);
  const int y1 = std::min(y0 + 1, source.height() - 1);
  const double fraction_x = bounded_x - static_cast<double>(x0);
  const double fraction_y = bounded_y - static_cast<double>(y0);
  const std::array<int, 4> sample_x{x0, x1, x0, x1};
  const std::array<int, 4> sample_y{y0, y0, y1, y1};
  const std::array<double, 4> weight{(1.0 - fraction_x) * (1.0 - fraction_y),
                                     fraction_x * (1.0 - fraction_y),
                                     (1.0 - fraction_x) * fraction_y, fraction_x * fraction_y};
  std::array<float, 4> sampled{};
  for (std::size_t tap = 0; tap < weight.size(); ++tap) {
    if (!inside_crop(sample_x[tap], sample_y[tap], source, transform)) {
      continue;
    }
    const auto pixel = source.pixel(sample_x[tap], sample_y[tap]);
    for (std::size_t channel = 0; channel < sampled.size(); ++channel) {
      sampled[channel] += static_cast<float>(static_cast<double>(pixel[channel]) * weight[tap]);
    }
  }
  return sampled;
}

void blend_premultiplied(const std::array<float, 4>& sampled, const float opacity,
                         const edit::BlendMode mode, std::span<float, 4> destination) {
  const float original_source_alpha = std::clamp(sampled[3], 0.0F, 1.0F);
  const float source_alpha = original_source_alpha * opacity;
  const float destination_alpha = std::clamp(destination[3], 0.0F, 1.0F);
  for (std::size_t channel = 0; channel < 3U; ++channel) {
    const float source_premultiplied = sampled[channel] * opacity;
    const float source_straight =
        original_source_alpha > 0.0F
            ? std::clamp(sampled[channel] / original_source_alpha, 0.0F, 1.0F)
            : 0.0F;
    const float destination_straight =
        destination_alpha > 0.0F ? std::clamp(destination[channel] / destination_alpha, 0.0F, 1.0F)
                                 : 0.0F;
    const float blended = blend_channel(mode, source_straight, destination_straight);
    destination[channel] = std::clamp(((1.0F - source_alpha) * destination[channel]) +
                                          ((1.0F - destination_alpha) * source_premultiplied) +
                                          (source_alpha * destination_alpha * blended),
                                      0.0F, 1.0F);
  }
  destination[3] =
      std::clamp(source_alpha + destination_alpha - (source_alpha * destination_alpha), 0.0F, 1.0F);
}

void fill_color(CpuFrame& frame, const edit::ColorRgba& color) noexcept {
  frame.clear(static_cast<float>(color.red), static_cast<float>(color.green),
              static_cast<float>(color.blue), static_cast<float>(color.alpha));
}

[[nodiscard]] std::optional<std::string>
effect_string(const edit::Effect& effect, const std::string_view id, const edit::Time local_time) {
  const auto found = effect.parameters.find(id);
  if (found == effect.parameters.end()) {
    return std::nullopt;
  }
  const auto value = edit::evaluateEffectParameter(found->second, local_time);
  if (!value) {
    return std::nullopt;
  }
  if (const auto* text = std::get_if<std::string>(&*value)) {
    return *text;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<double>
effect_number(const edit::Effect& effect, const std::string_view id, const edit::Time local_time) {
  const auto found = effect.parameters.find(id);
  if (found == effect.parameters.end()) {
    return std::nullopt;
  }
  const auto value = edit::evaluateEffectParameter(found->second, local_time);
  if (!value) {
    return std::nullopt;
  }
  if (const auto* number = std::get_if<double>(&*value)) {
    return *number;
  }
  if (const auto* integer = std::get_if<std::int64_t>(&*value)) {
    return static_cast<double>(*integer);
  }
  return std::nullopt;
}

void apply_color(CpuFrame& frame, const edit::Effect& effect, const edit::Time local_time) {
  const double exposure = effect_number(effect, "exposure", local_time).value_or(0.0);
  const double contrast = effect_number(effect, "contrast", local_time).value_or(1.0);
  const double saturation = effect_number(effect, "saturation", local_time).value_or(1.0);
  const double temperature = effect_number(effect, "temperature", local_time).value_or(0.0);
  const double tint = effect_number(effect, "tint", local_time).value_or(0.0);
  const double exposure_scale = std::exp2(std::clamp(exposure, -32.0, 32.0));
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      auto pixel = frame.pixel(x, y);
      const double alpha = std::clamp(static_cast<double>(pixel[3]), 0.0, 1.0);
      if (alpha <= 0.0) {
        continue;
      }
      double red = std::clamp(static_cast<double>(pixel[0]) / alpha, 0.0, 1.0);
      double green = std::clamp(static_cast<double>(pixel[1]) / alpha, 0.0, 1.0);
      double blue = std::clamp(static_cast<double>(pixel[2]) / alpha, 0.0, 1.0);
      red *= exposure_scale;
      green *= exposure_scale;
      blue *= exposure_scale;
      red = ((red - 0.5) * contrast) + 0.5;
      green = ((green - 0.5) * contrast) + 0.5;
      blue = ((blue - 0.5) * contrast) + 0.5;
      const double luma = (0.2126 * red) + (0.7152 * green) + (0.0722 * blue);
      red = luma + ((red - luma) * saturation);
      green = luma + ((green - luma) * saturation);
      blue = luma + ((blue - luma) * saturation);
      // Temperature and tint are intentionally small normalized channel
      // offsets. This is the CPU reference transform; the GPU path must use
      // the same canonical parameter values when its shader support lands.
      red += temperature * 0.1 + tint * 0.05;
      green -= tint * 0.1;
      blue -= temperature * 0.1 + tint * 0.05;
      pixel[0] = static_cast<float>(std::clamp(red, 0.0, 1.0) * alpha);
      pixel[1] = static_cast<float>(std::clamp(green, 0.0, 1.0) * alpha);
      pixel[2] = static_cast<float>(std::clamp(blue, 0.0, 1.0) * alpha);
    }
  }
}

void apply_lut(CpuFrame& frame, const edit::Effect& effect, const edit::Time local_time) {
  const auto path = effect_string(effect, "path", local_time);
  if (!path || path->empty()) {
    return;
  }
  const Lut3D* lut = cached_lut_for_path(std::filesystem::path{*path});
  if (lut == nullptr) {
    return;
  }
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      auto pixel = frame.pixel(x, y);
      const double alpha = std::clamp(static_cast<double>(pixel[3]), 0.0, 1.0);
      if (alpha <= 0.0) {
        continue;
      }
      const double red = std::clamp(static_cast<double>(pixel[0]) / alpha, 0.0, 1.0);
      const double green = std::clamp(static_cast<double>(pixel[1]) / alpha, 0.0, 1.0);
      const double blue = std::clamp(static_cast<double>(pixel[2]) / alpha, 0.0, 1.0);
      const auto mapped = lut->sample(static_cast<float>(red), static_cast<float>(green),
                                      static_cast<float>(blue));
      pixel[0] = static_cast<float>(std::clamp(static_cast<double>(mapped[0]), 0.0, 1.0) * alpha);
      pixel[1] = static_cast<float>(std::clamp(static_cast<double>(mapped[1]), 0.0, 1.0) * alpha);
      pixel[2] = static_cast<float>(std::clamp(static_cast<double>(mapped[2]), 0.0, 1.0) * alpha);
    }
  }
}

void apply_curves(CpuFrame& frame, const edit::Effect& effect, const edit::Time local_time) {
  const auto red = effect_string(effect, "red", local_time).value_or("0,0;1,1");
  const auto green = effect_string(effect, "green", local_time).value_or("0,0;1,1");
  const auto blue = effect_string(effect, "blue", local_time).value_or("0,0;1,1");
  const auto luma = effect_string(effect, "luma", local_time).value_or("0,0;1,1");
  const auto curves = parse_color_curves(red, green, blue, luma);
  if (!curves) {
    return;
  }
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      auto pixel = frame.pixel(x, y);
      const double alpha = std::clamp(static_cast<double>(pixel[3]), 0.0, 1.0);
      if (alpha <= 0.0) {
        continue;
      }
      float straight_red = static_cast<float>(std::clamp(static_cast<double>(pixel[0]) / alpha, 0.0, 1.0));
      float straight_green =
          static_cast<float>(std::clamp(static_cast<double>(pixel[1]) / alpha, 0.0, 1.0));
      float straight_blue =
          static_cast<float>(std::clamp(static_cast<double>(pixel[2]) / alpha, 0.0, 1.0));
      apply_color_curves(straight_red, straight_green, straight_blue, *curves);
      pixel[0] = straight_red * static_cast<float>(alpha);
      pixel[1] = straight_green * static_cast<float>(alpha);
      pixel[2] = straight_blue * static_cast<float>(alpha);
    }
  }
}

void apply_box_blur(CpuFrame& frame, const double requested_radius) {
  const int radius = std::clamp(static_cast<int>(std::ceil(requested_radius)), 0, 64);
  if (radius == 0) {
    return;
  }
  CpuFrame blurred(frame.width(), frame.height());
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      std::array<float, 4> sum{};
      int count = 0;
      for (int sample_y = std::max(0, y - radius);
           sample_y <= std::min(frame.height() - 1, y + radius); ++sample_y) {
        for (int sample_x = std::max(0, x - radius);
             sample_x <= std::min(frame.width() - 1, x + radius); ++sample_x) {
          const auto sample = frame.pixel(sample_x, sample_y);
          for (std::size_t channel = 0; channel < sum.size(); ++channel) {
            sum[channel] += sample[channel];
          }
          ++count;
        }
      }
      auto output = blurred.pixel(x, y);
      for (std::size_t channel = 0; channel < sum.size(); ++channel) {
        output[channel] = sum[channel] / static_cast<float>(count);
      }
    }
  }
  std::copy(blurred.pixels().begin(), blurred.pixels().end(), frame.pixels().begin());
}

void apply_visual_effects(RenderedClip& rendered, const edit::Time local_time,
                          const PreviewProfile& profile) {
  apply_clip_visual_effects(*rendered.frame, rendered.clip, local_time, profile);
}

void composite(const CpuFrame& source, CpuFrame& destination, const edit::Clip& clip,
               const std::uint32_t sequence_width, const std::uint32_t sequence_height) {
  const auto& transform = clip.transform;
  const double preview_x =
      static_cast<double>(destination.width()) / static_cast<double>(sequence_width);
  const double preview_y =
      static_cast<double>(destination.height()) / static_cast<double>(sequence_height);
  const double destination_anchor_x =
      (static_cast<double>(destination.width() - 1) * 0.5) + transform.position.x * preview_x;
  const double destination_anchor_y =
      (static_cast<double>(destination.height() - 1) * 0.5) + transform.position.y * preview_y;
  const double source_anchor_x = transform.anchor_x * static_cast<double>(source.width() - 1);
  const double source_anchor_y = transform.anchor_y * static_cast<double>(source.height() - 1);
  const double radians = transform.rotation_degrees * std::numbers::pi / 180.0;
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  const float opacity = static_cast<float>(transform.opacity);

  for (int destination_y = 0; destination_y < destination.height(); ++destination_y) {
    const double delta_y = static_cast<double>(destination_y) - destination_anchor_y;
    for (int destination_x = 0; destination_x < destination.width(); ++destination_x) {
      const double delta_x = static_cast<double>(destination_x) - destination_anchor_x;
      const double unrotated_x = cosine * delta_x + sine * delta_y;
      const double unrotated_y = -sine * delta_x + cosine * delta_y;
      const double source_x = source_anchor_x + unrotated_x / transform.scale.x;
      const double source_y = source_anchor_y + unrotated_y / transform.scale.y;
      const auto sampled = sample_bilinear(source, source_x, source_y, transform);
      if (sampled[3] <= 0.0F || opacity <= 0.0F) {
        continue;
      }
      blend_premultiplied(sampled, opacity, clip.blend_mode,
                          destination.pixel(destination_x, destination_y));
    }
  }
}


[[nodiscard]] TitleStyle title_style_for(const edit::Clip& clip, const edit::Sequence& sequence,
                                         const PreviewProfile& profile) {
  TitleStyle style;
  style.text = !clip.name.empty() ? clip.name : "TITLE";
  style.width = scaled_dimension(sequence.width, profile.scale);
  style.height = scaled_dimension(sequence.height, profile.scale);
  if (clip.title.has_value()) {
    const auto& title = *clip.title;
    style.text = title.text;
    style.font_family = title.font_family;
    style.foreground = title.foreground_color;
    style.background = title.background_color;
    style.alignment = title.horizontal_alignment;
    style.font_size = title.font_size;
    style.bold = title.bold;
    style.italic = title.italic;
  }
  style.width = std::max(1, style.width);
  style.height = std::max(1, style.height);
  return style;
}

[[nodiscard]] std::shared_ptr<const CpuFrame> detail_rasterize_title_frame(
    const edit::Clip& clip, const edit::Sequence& sequence, const PreviewProfile& profile) {
  const TitleStyle style = title_style_for(clip, sequence, profile);
  auto frame = std::make_shared<CpuFrame>(style.width, style.height);
  fill_color(*frame, style.background);

  if (style.text.empty()) {
    return frame;
  }

  const double preview_font_size =
      style.font_size / static_cast<double>(preview_scale_divisor(profile.scale));
  double draw_font_size = preview_font_size;
  TextBlockMetrics metrics =
      measure_text_block(style.text, style.font_family, draw_font_size, style.bold, style.italic);
  if (metrics.width > 0 && metrics.height > 0) {
    const double width_scale = static_cast<double>(frame->width()) / static_cast<double>(metrics.width);
    const double height_scale =
        static_cast<double>(frame->height()) / static_cast<double>(metrics.height);
    const double fit = std::min(1.0, std::min(width_scale, height_scale));
    if (fit < 1.0) {
      draw_font_size = preview_font_size * fit;
      metrics =
          measure_text_block(style.text, style.font_family, draw_font_size, style.bold, style.italic);
    }
  }
  const int content_height = metrics.height;
  const int top = std::max(0, (frame->height() - content_height) / 2);

  const auto lines = [&style]() {
    std::vector<std::string> split(1);
    for (std::size_t index = 0; index < style.text.size(); ++index) {
      if (style.text[index] == '\n') {
        split.emplace_back();
        continue;
      }
      split.back().push_back(style.text[index]);
    }
    if (split.empty()) {
      split.emplace_back();
    }
    return split;
  }();

  TextHorizontalAlignment text_alignment = TextHorizontalAlignment::Center;
  switch (style.alignment) {
  case edit::TitleHorizontalAlignment::Left:
    text_alignment = TextHorizontalAlignment::Left;
    break;
  case edit::TitleHorizontalAlignment::Center:
    text_alignment = TextHorizontalAlignment::Center;
    break;
  case edit::TitleHorizontalAlignment::Right:
    text_alignment = TextHorizontalAlignment::Right;
    break;
  }

  int line_y = top;
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const int line_width =
        line_index < metrics.line_widths.size() ? metrics.line_widths[line_index] : 0;
    int left = 0;
    switch (text_alignment) {
    case TextHorizontalAlignment::Left:
      left = 0;
      break;
    case TextHorizontalAlignment::Center:
      left = std::max(0, (frame->width() - line_width) / 2);
      break;
    case TextHorizontalAlignment::Right:
      left = std::max(0, frame->width() - line_width);
      break;
    }
    draw_text_line(*frame, lines[line_index], style.font_family, left, line_y, draw_font_size,
                   style.bold, style.italic, style.foreground);
    line_y += metrics.line_height;
  }
  return frame;
}

[[nodiscard]] const edit::Clip* find_clip_on_track(const edit::Track& track,
                                                   const edit::EntityId clip_id) {
  const auto found =
      std::find_if(track.clips.begin(), track.clips.end(),
                   [&clip_id](const edit::Clip& clip) { return clip.id == clip_id; });
  return found == track.clips.end() ? nullptr : &*found;
}

[[nodiscard]] std::shared_ptr<CpuFrame> clone_frame(const CpuFrame& source) {
  auto frame = std::make_shared<CpuFrame>(source.width(), source.height());
  std::copy(source.pixels().begin(), source.pixels().end(), frame->pixels().begin());
  return frame;
}

[[nodiscard]] RenderResult<RenderedClip>
render_clip_source(const edit::Clip& clip, const edit::Sequence& sequence, const edit::Time time,
                   const PreviewProfile& profile, const std::uint64_t request_epoch,
                   FrameProvider& provider, const CpuRenderer& renderer) {
  if (clip.kind == edit::ClipKind::Title) {
    auto frame = std::make_shared<CpuFrame>(*detail_rasterize_title_frame(clip, sequence, profile));
    RenderedClip rendered{.frame = std::move(frame), .clip = clip};
    apply_visual_effects(rendered, time - clip.timeline_range.start, profile);
    return RenderResult<RenderedClip>::success(std::move(rendered));
  }

  AssetFrameRequest request{
      .asset_id = clip.asset_id,
      .source_time = source_time_for(clip, time),
      .preferred_width = scaled_dimension(sequence.width, profile.scale),
      .preferred_height = scaled_dimension(sequence.height, profile.scale),
      .permit_proxy = profile.use_proxies,
      .request_epoch = request_epoch,
  };
  auto source = provider.request(request);
  if (!source) {
    return RenderResult<RenderedClip>::failure(*source.error);
  }
  if (request_epoch != renderer.current_epoch()) {
    return RenderResult<RenderedClip>::failure(
        {.code = RenderErrorCode::StaleRequest,
         .message = "render request was superseded during decoding"});
  }
  auto frame = clone_frame(**source.value);
  RenderedClip rendered{.frame = std::move(frame), .clip = clip};
  apply_visual_effects(rendered, time - clip.timeline_range.start, profile);
  return RenderResult<RenderedClip>::success(std::move(rendered));
}

[[nodiscard]] RenderResult<std::shared_ptr<CpuFrame>>
render_track_clip_over_baseline(const edit::Clip& clip, const edit::Sequence& sequence,
                                const edit::Time time, const PreviewProfile& profile,
                                const std::uint64_t request_epoch, FrameProvider& provider,
                                const CpuRenderer& renderer, const CpuFrame& baseline) {
  auto rendered = clone_frame(baseline);
  auto source =
      render_clip_source(clip, sequence, time, profile, request_epoch, provider, renderer);
  if (!source) {
    return RenderResult<std::shared_ptr<CpuFrame>>::failure(*source.error);
  }
  composite(*source.value->frame, *rendered, source.value->clip, sequence.width, sequence.height);
  return RenderResult<std::shared_ptr<CpuFrame>>::success(std::move(rendered));
}

[[nodiscard]] std::shared_ptr<CpuFrame> black_frame_like(const CpuFrame& source) {
  auto black = std::make_shared<CpuFrame>(source.width(), source.height());
  black->clear(0.0F, 0.0F, 0.0F, 1.0F);
  return black;
}

} // namespace

float cpu_timeline_saturate(const double value) noexcept {
  return saturate(value);
}

double cpu_timeline_time_ratio(const edit::Time numerator, const edit::Time denominator) {
  return time_ratio(numerator, denominator);
}

bool clip_has_unsupported_gpu_effects(const std::vector<edit::Effect>& effects) {
  return std::any_of(effects.begin(), effects.end(), [](const edit::Effect& effect) {
    if (!effect.enabled) {
      return false;
    }
    return effect.type != "video.color" && effect.type != "video.gaussian_blur" &&
           effect.type != "video.crop";
  });
}

void composite_clip_onto_frame(const CpuFrame& source, CpuFrame& destination, const edit::Clip& clip,
                               const std::uint32_t sequence_width,
                               const std::uint32_t sequence_height) {
  composite(source, destination, clip, sequence_width, sequence_height);
}

void composite_blend_frame(CpuFrame& destination, const CpuFrame& source,
                           const edit::BlendMode blend_mode) {
  for (int y = 0; y < destination.height(); ++y) {
    for (int x = 0; x < destination.width(); ++x) {
      const auto sampled = source.pixel(x, y);
      if (sampled[3] <= 0.0F) {
        continue;
      }
      std::array<float, 4> sampled_array{
          sampled[0],
          sampled[1],
          sampled[2],
          sampled[3],
      };
      blend_premultiplied(sampled_array, 1.0F, blend_mode, destination.pixel(x, y));
    }
  }
}

std::shared_ptr<CpuFrame> rasterize_title_frame(const edit::Clip& clip,
                                                const edit::Sequence& sequence,
                                                const PreviewProfile& profile) {
  return std::make_shared<CpuFrame>(*detail_rasterize_title_frame(clip, sequence, profile));
}

void apply_clip_visual_effects(CpuFrame& frame, edit::Clip& clip, const edit::Time local_time,
                               const PreviewProfile& profile) {
  for (const auto& effect : clip.effects) {
    if (!effect.enabled || !effect.known) {
      continue;
    }
    if (effect.type == "video.color") {
      apply_color(frame, effect, local_time);
    } else if (effect.type == "video.gaussian_blur") {
      if (!profile.bypass_expensive_effects) {
        apply_box_blur(frame, effect_number(effect, "radius", local_time).value_or(0.0));
      }
    } else if (effect.type == "video.lut") {
      apply_lut(frame, effect, local_time);
    } else if (effect.type == "video.curves") {
      apply_curves(frame, effect, local_time);
    } else if (effect.type == "video.crop") {
      const double left = effect_number(effect, "left", local_time).value_or(0.0);
      const double top = effect_number(effect, "top", local_time).value_or(0.0);
      const double right = effect_number(effect, "right", local_time).value_or(0.0);
      const double bottom = effect_number(effect, "bottom", local_time).value_or(0.0);
      if (left >= 0.0 && top >= 0.0 && right >= 0.0 && bottom >= 0.0 && left + right < 1.0 &&
          top + bottom < 1.0) {
        const auto base_width = 1.0 - clip.transform.crop_left - clip.transform.crop_right;
        const auto base_height = 1.0 - clip.transform.crop_top - clip.transform.crop_bottom;
        clip.transform.crop_left += base_width * left;
        clip.transform.crop_right += base_width * right;
        clip.transform.crop_top += base_height * top;
        clip.transform.crop_bottom += base_height * bottom;
      }
    }
  }
}

std::optional<ActiveTransitionInfo> active_transition_for_track(const edit::Sequence& sequence,
                                                                const edit::Track& track,
                                                                const edit::Time time) {
  for (const edit::Transition& transition : sequence.transitions) {
    if (!transition.enabled || !transition.range.contains(time)) {
      continue;
    }
    const edit::Clip* outgoing = find_clip_on_track(track, transition.outgoing_clip_id);
    const edit::Clip* incoming = find_clip_on_track(track, transition.incoming_clip_id);
    if (outgoing == nullptr || incoming == nullptr) {
      continue;
    }
    return ActiveTransitionInfo{
        .transition = &transition,
        .outgoing = outgoing,
        .incoming = incoming,
    };
  }
  return std::nullopt;
}

std::shared_ptr<CpuFrame> blend_frames(const CpuFrame& left, const CpuFrame& right,
                                       const float factor) {
  auto output = std::make_shared<CpuFrame>(left.width(), left.height());
  const float clamped = std::clamp(factor, 0.0F, 1.0F);
  for (int y = 0; y < left.height(); ++y) {
    for (int x = 0; x < left.width(); ++x) {
      const auto lhs = left.pixel(x, y);
      const auto rhs = right.pixel(x, y);
      auto destination = output->pixel(x, y);
      for (std::size_t channel = 0; channel < 4U; ++channel) {
        destination[channel] = ((1.0F - clamped) * lhs[channel]) + (clamped * rhs[channel]);
      }
    }
  }
  return output;
}

std::shared_ptr<CpuFrame> opaque_black_frame_like(const CpuFrame& source) {
  return black_frame_like(source);
}

CpuRenderer::CpuRenderer(std::shared_ptr<FrameProvider> provider) : provider_(std::move(provider)) {
  if (!provider_) {
    throw std::invalid_argument("CPU renderer requires a frame provider");
  }
}

void CpuRenderer::begin_epoch(const std::uint64_t request_epoch) noexcept {
  epoch_.store(request_epoch, std::memory_order_release);
}

std::uint64_t CpuRenderer::current_epoch() const noexcept {
  return epoch_.load(std::memory_order_acquire);
}

RenderResult<VideoFrame> CpuRenderer::request_frame(const edit::TimelineSnapshot& snapshot,
                                                    const edit::Time time,
                                                    const PreviewProfile& profile,
                                                    const std::uint64_t request_epoch) const {
  if (request_epoch != current_epoch()) {
    return RenderResult<VideoFrame>::failure(
        {.code = RenderErrorCode::StaleRequest,
         .message = "render request belongs to a stale epoch"});
  }

  const edit::Sequence* sequence = nullptr;
  try {
    sequence = &snapshot.sequence();
  } catch (const std::exception& exception) {
    return RenderResult<VideoFrame>::failure(
        {.code = RenderErrorCode::InvalidSnapshot, .message = exception.what()});
  }
  if (time.isNegative()) {
    return RenderResult<VideoFrame>::failure(
        {.code = RenderErrorCode::InvalidTime, .message = "cannot render negative timeline time"});
  }

  const int width = scaled_dimension(sequence->width, profile.scale);
  const int height = scaled_dimension(sequence->height, profile.scale);
  auto output = std::make_shared<CpuFrame>(width, height);
  output->clear(0.0F, 0.0F, 0.0F, 1.0F);

  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Video || track.muted || !track.visible) {
      continue;
    }
    const auto transition = active_transition_for_track(*sequence, track, time);
    if (transition.has_value()) {
      auto outgoing =
          render_track_clip_over_baseline(*transition->outgoing, *sequence, time, profile,
                                          request_epoch, *provider_, *this, *output);
      if (!outgoing) {
        return RenderResult<VideoFrame>::failure(*outgoing.error);
      }
      auto incoming =
          render_track_clip_over_baseline(*transition->incoming, *sequence, time, profile,
                                          request_epoch, *provider_, *this, *output);
      if (!incoming) {
        return RenderResult<VideoFrame>::failure(*incoming.error);
      }
      const edit::Time cut_time = transition->incoming->timeline_range.start;
      if (transition->transition->kind == edit::TransitionKind::CrossDissolve) {
        const float factor = saturate(time_ratio(time - transition->transition->range.start,
                                                 transition->transition->range.duration));
        output = blend_frames(**outgoing.value, **incoming.value, factor);
      } else if (time < cut_time) {
        const float factor = saturate(time_ratio(time - transition->transition->range.start,
                                                 cut_time - transition->transition->range.start));
        output = blend_frames(**outgoing.value, *black_frame_like(**outgoing.value), factor);
      } else {
        const float factor =
            saturate(time_ratio(time - cut_time, transition->transition->range.end() - cut_time));
        output = blend_frames(*black_frame_like(**incoming.value), **incoming.value, factor);
      }
      continue;
    }
    for (const edit::Clip& clip : track.clips) {
      if (!clip.timeline_range.contains(time)) {
        continue;
      }
      auto source =
          render_clip_source(clip, *sequence, time, profile, request_epoch, *provider_, *this);
      if (!source) {
        return RenderResult<VideoFrame>::failure(*source.error);
      }
      composite(*source.value->frame, *output, source.value->clip, sequence->width,
                sequence->height);
    }
  }

  const edit::Time frame_duration = sequence->frame_rate.frameTime();
  VideoFrame frame{
      .timestamp = time,
      .duration = frame_duration,
      .width = width,
      .height = height,
      .layout = PixelLayout::RgbaFloat32,
      .bit_depth = 32,
      .color = {},
      .field_order = "progressive",
      .sample_aspect_ratio = edit::Time(1, 1),
      .orientation_degrees = 0,
      .alpha_mode = AlphaMode::Premultiplied,
      .storage = std::static_pointer_cast<const CpuFrame>(output),
  };
  return RenderResult<VideoFrame>::success(std::move(frame));
}

} // namespace video_editor::render
