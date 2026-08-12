// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/bitmap_glyphs.h"
#include "video_editor/render_engine/cpu_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
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

struct ActiveTransition final {
  const edit::Transition* transition{nullptr};
  const edit::Clip* outgoing{nullptr};
  const edit::Clip* incoming{nullptr};
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

[[nodiscard]] std::vector<char32_t> decode_utf8_with_replacement(std::string_view text) {
  return render::decode_utf8_with_replacement(text);
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

[[nodiscard]] std::shared_ptr<const CpuFrame> rasterize_title_frame(const edit::Clip& clip,
                                                                    const edit::Sequence& sequence,
                                                                    const PreviewProfile& profile) {
  const TitleStyle style = title_style_for(clip, sequence, profile);
  auto frame = std::make_shared<CpuFrame>(style.width, style.height);
  fill_color(*frame, style.background);

  const std::vector<char32_t> decoded = decode_utf8_with_replacement(style.text);
  std::vector<std::vector<char32_t>> lines(1);
  for (const char32_t codepoint : decoded) {
    if (codepoint == U'\n') {
      lines.emplace_back();
      continue;
    }
    lines.back().push_back(render::supported_glyph(codepoint) ? codepoint : U'\uFFFD');
  }
  if (lines.empty()) {
    lines.emplace_back();
  }

  std::size_t max_columns = 0;
  for (const auto& line : lines) {
    max_columns = std::max(max_columns, line.size());
  }
  const int cell_width = 6;
  const int cell_height = 8;
  const int content_width = static_cast<int>(std::max<std::size_t>(1U, max_columns) * cell_width);
  const int content_height = static_cast<int>(lines.size() * cell_height);
  const double preview_font_size =
      style.font_size / static_cast<double>(preview_scale_divisor(profile.scale));
  const int requested_scale = std::max(1, static_cast<int>(std::lround(preview_font_size / 8.0)));
  const int fitted_scale = std::max(1, std::min(frame->width() / std::max(1, content_width),
                                                frame->height() / std::max(1, content_height)));
  const int scale = std::min(requested_scale, fitted_scale);
  const int italic_padding = style.italic ? std::max(1, (2 * scale)) : 0;
  const int scaled_content_height = content_height * scale;
  const int top = std::max(0, (frame->height() - scaled_content_height) / 2);

  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    const auto& line = lines[line_index];
    const int line_width = (static_cast<int>(line.size()) * cell_width * scale) + italic_padding;
    int left = 0;
    switch (style.alignment) {
    case edit::TitleHorizontalAlignment::Left:
      left = 0;
      break;
    case edit::TitleHorizontalAlignment::Center:
      left = std::max(0, (frame->width() - line_width) / 2);
      break;
    case edit::TitleHorizontalAlignment::Right:
      left = std::max(0, frame->width() - line_width);
      break;
    }
    for (std::size_t glyph_index = 0; glyph_index < line.size(); ++glyph_index) {
      const auto glyph = line[glyph_index] == U'\uFFFD' ? render::replacement_glyph()
                                                       : render::glyph_for_ascii(line[glyph_index]);
      render::draw_glyph(*frame, glyph, left + static_cast<int>(glyph_index * cell_width) * scale,
                 top + static_cast<int>(line_index * cell_height) * scale, scale, style.foreground,
                 style.bold, style.italic);
    }
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

[[nodiscard]] std::optional<ActiveTransition>
active_transition_for_track(const edit::Sequence& sequence, const edit::Track& track,
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
    return ActiveTransition{
        .transition = &transition,
        .outgoing = outgoing,
        .incoming = incoming,
    };
  }
  return std::nullopt;
}

[[nodiscard]] std::shared_ptr<CpuFrame> clone_frame(const CpuFrame& source) {
  auto frame = std::make_shared<CpuFrame>(source.width(), source.height());
  std::copy(source.pixels().begin(), source.pixels().end(), frame->pixels().begin());
  return frame;
}

[[nodiscard]] RenderResult<std::shared_ptr<const CpuFrame>>
render_clip_source(const edit::Clip& clip, const edit::Sequence& sequence, const edit::Time time,
                   const PreviewProfile& profile, const std::uint64_t request_epoch,
                   FrameProvider& provider, const CpuRenderer& renderer) {
  if (clip.kind == edit::ClipKind::Title) {
    return RenderResult<std::shared_ptr<const CpuFrame>>::success(
        rasterize_title_frame(clip, sequence, profile));
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
    return RenderResult<std::shared_ptr<const CpuFrame>>::failure(*source.error);
  }
  if (request_epoch != renderer.current_epoch()) {
    return RenderResult<std::shared_ptr<const CpuFrame>>::failure(
        {.code = RenderErrorCode::StaleRequest,
         .message = "render request was superseded during decoding"});
  }
  return source;
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
  composite(**source.value, *rendered, clip, sequence.width, sequence.height);
  return RenderResult<std::shared_ptr<CpuFrame>>::success(std::move(rendered));
}

[[nodiscard]] std::shared_ptr<CpuFrame> blend_frames(const CpuFrame& left, const CpuFrame& right,
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

[[nodiscard]] std::shared_ptr<CpuFrame> black_frame_like(const CpuFrame& source) {
  auto black = std::make_shared<CpuFrame>(source.width(), source.height());
  black->clear(0.0F, 0.0F, 0.0F, 1.0F);
  return black;
}

} // namespace

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
      composite(**source.value, *output, clip, sequence->width, sequence->height);
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
