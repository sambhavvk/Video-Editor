// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/cpu_renderer.h"

#include <algorithm>
#include <cmath>
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

void composite(const CpuFrame& source, CpuFrame& destination, const edit::Clip& clip) {
  const double scale_x = std::max(std::abs(clip.transform.scale.x), 0.0001);
  const double scale_y = std::max(std::abs(clip.transform.scale.y), 0.0001);
  const int displayed_width = std::max(1, static_cast<int>(std::lround(source.width() * scale_x)));
  const int displayed_height = std::max(1, static_cast<int>(std::lround(source.height() * scale_y)));
  const int origin_x = ((destination.width() - displayed_width) / 2) +
                       static_cast<int>(std::lround(clip.transform.position.x));
  const int origin_y = ((destination.height() - displayed_height) / 2) +
                       static_cast<int>(std::lround(clip.transform.position.y));
  const float opacity = static_cast<float>(std::clamp(clip.transform.opacity, 0.0, 1.0));

  const int crop_left = static_cast<int>(std::clamp(clip.transform.crop_left, 0.0, 1.0) *
                                         static_cast<double>(displayed_width));
  const int crop_right = static_cast<int>(std::clamp(clip.transform.crop_right, 0.0, 1.0) *
                                          static_cast<double>(displayed_width));
  const int crop_top = static_cast<int>(std::clamp(clip.transform.crop_top, 0.0, 1.0) *
                                        static_cast<double>(displayed_height));
  const int crop_bottom = static_cast<int>(std::clamp(clip.transform.crop_bottom, 0.0, 1.0) *
                                           static_cast<double>(displayed_height));

  for (int output_y = crop_top; output_y < displayed_height - crop_bottom; ++output_y) {
    const int destination_y = origin_y + output_y;
    if (destination_y < 0 || destination_y >= destination.height()) {
      continue;
    }
    const int source_y = std::clamp((output_y * source.height()) / displayed_height, 0,
                                    source.height() - 1);
    for (int output_x = crop_left; output_x < displayed_width - crop_right; ++output_x) {
      const int destination_x = origin_x + output_x;
      if (destination_x < 0 || destination_x >= destination.width()) {
        continue;
      }
      const int source_x = std::clamp((output_x * source.width()) / displayed_width, 0,
                                      source.width() - 1);
      const auto source_pixel = source.pixel(source_x, source_y);
      auto destination_pixel = destination.pixel(destination_x, destination_y);
      const float source_alpha = std::clamp(source_pixel[3] * opacity, 0.0F, 1.0F);
      const float inverse_alpha = 1.0F - source_alpha;
      for (std::size_t channel = 0; channel < 3U; ++channel) {
        const float blended = blend_channel(clip.blend_mode, source_pixel[channel],
                                            destination_pixel[channel]);
        destination_pixel[channel] = (blended * source_alpha) +
                                     (destination_pixel[channel] * inverse_alpha);
      }
      destination_pixel[3] = source_alpha + (destination_pixel[3] * inverse_alpha);
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
        {.code = RenderErrorCode::StaleRequest, .message = "render request belongs to a stale epoch"});
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
      composite(**source.value, *output, clip);
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

