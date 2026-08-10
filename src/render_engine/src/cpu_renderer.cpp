// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/cpu_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace video_editor::render {
namespace {

int scaled_dimension(const std::uint32_t value, const PreviewScale scale) {
  const int divisor = scale == PreviewScale::Full ? 1 : scale == PreviewScale::Half ? 2 : 4;
  return std::max(1, static_cast<int>(value) / divisor);
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
    if (track.kind != edit::TrackKind::Video || track.muted) {
      continue;
    }
    for (const edit::Clip& clip : track.clips) {
      if (!clip.timeline_range.contains(time)) {
        continue;
      }
      AssetFrameRequest request{
          .asset_id = clip.asset_id,
          .source_time = source_time_for(clip, time),
          .preferred_width = width,
          .preferred_height = height,
          .permit_proxy = profile.use_proxies,
          .request_epoch = request_epoch,
      };
      auto source = provider_->request(request);
      if (!source) {
        return RenderResult<VideoFrame>::failure(*source.error);
      }
      if (request_epoch != current_epoch()) {
        return RenderResult<VideoFrame>::failure(
            {.code = RenderErrorCode::StaleRequest,
             .message = "render request was superseded during decoding"});
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
