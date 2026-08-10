// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/gpu_timeline_renderer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace video_editor::render {
namespace {

[[nodiscard]] int scaled_dimension(const std::uint32_t value, const PreviewScale scale) {
  const int divisor = scale == PreviewScale::Full ? 1 : scale == PreviewScale::Half ? 2 : 4;
  return std::max(1, static_cast<int>(value) / divisor);
}

[[nodiscard]] edit::Time source_time_for(const edit::Clip& clip, const edit::Time timeline_time) {
  edit::Time offset = timeline_time - clip.timeline_range.start;
  offset = offset.scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
                         edit::RoundingMode::NearestTiesEven);
  return clip.reversed ? clip.source_range.end() - offset : clip.source_range.start + offset;
}

[[nodiscard]] bool has_enabled_effects(const std::vector<edit::Effect>& effects) {
  return std::any_of(effects.begin(), effects.end(),
                     [](const edit::Effect& effect) { return effect.enabled; });
}

[[nodiscard]] RenderResult<GpuImage> stale() {
  return RenderResult<GpuImage>::failure(
      {.code = RenderErrorCode::StaleRequest,
       .message = "GPU timeline request belongs to a stale epoch"});
}

} // namespace

GpuTimelineRenderer::GpuTimelineRenderer(std::shared_ptr<FrameProvider> provider,
                                         std::shared_ptr<GpuRenderer> renderer)
    : provider_(std::move(provider)), renderer_(std::move(renderer)) {
  if (!provider_) {
    throw std::invalid_argument("GPU timeline renderer requires a frame provider");
  }
  if (!renderer_) {
    throw std::invalid_argument("GPU timeline renderer requires a GPU renderer");
  }
}

void GpuTimelineRenderer::begin_epoch(const std::uint64_t request_epoch) noexcept {
  epoch_.store(request_epoch, std::memory_order_release);
}

std::uint64_t GpuTimelineRenderer::current_epoch() const noexcept {
  return epoch_.load(std::memory_order_acquire);
}

RenderResult<GpuImage> GpuTimelineRenderer::request_frame(const edit::TimelineSnapshot& snapshot,
                                                          const edit::Time time,
                                                          const PreviewProfile& profile,
                                                          const std::uint64_t request_epoch) const {
  if (request_epoch != current_epoch()) {
    return stale();
  }

  const edit::Sequence* sequence = nullptr;
  try {
    sequence = &snapshot.sequence();
  } catch (const std::exception& exception) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::InvalidSnapshot, .message = exception.what()});
  }
  if (time.isNegative()) {
    return RenderResult<GpuImage>::failure(
        {.code = RenderErrorCode::InvalidTime, .message = "cannot render negative timeline time"});
  }

  const int width = scaled_dimension(sequence->width, profile.scale);
  const int height = scaled_dimension(sequence->height, profile.scale);
  std::vector<GpuLayer> layers;

  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Video || track.muted) {
      continue;
    }
    if (has_enabled_effects(track.effects)) {
      return RenderResult<GpuImage>::failure(
          {.code = RenderErrorCode::GpuUnsupportedTimeline,
           .message = "GPU timeline preview does not yet support enabled track effects"});
    }
    for (const edit::Clip& clip : track.clips) {
      if (!clip.timeline_range.contains(time)) {
        continue;
      }
      if (clip.kind != edit::ClipKind::Video || has_enabled_effects(clip.effects)) {
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuUnsupportedTimeline,
             .message = clip.kind != edit::ClipKind::Video
                            ? "GPU timeline preview does not yet support title clips"
                            : "GPU timeline preview does not yet support enabled clip effects"});
      }
      if (clip.blend_mode != edit::BlendMode::Normal) {
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuUnsupportedTimeline,
             .message = "GPU timeline preview currently supports only Normal blend mode"});
      }
      if (!std::isfinite(clip.transform.scale.x) || !std::isfinite(clip.transform.scale.y) ||
          clip.transform.scale.x == 0.0 || clip.transform.scale.y == 0.0) {
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuUnsupportedTimeline,
             .message = "GPU timeline preview requires finite, non-zero clip scale"});
      }
      if (std::abs(clip.transform.rotation_degrees) > 1.0e-9 &&
          (std::abs(clip.transform.position.x) > 1.0e-9 ||
           std::abs(clip.transform.position.y) > 1.0e-9 ||
           std::abs(clip.transform.anchor_x - 0.5) > 1.0e-9 ||
           std::abs(clip.transform.anchor_y - 0.5) > 1.0e-9)) {
        return RenderResult<GpuImage>::failure(
            {.code = RenderErrorCode::GpuUnsupportedTimeline,
             .message = "GPU timeline preview does not yet combine rotation with a moved pivot; "
                        "use the CPU fallback for this frame"});
      }

      AssetFrameRequest request{
          .asset_id = clip.asset_id,
          .source_time = source_time_for(clip, time),
          .preferred_width = width,
          .preferred_height = height,
          .permit_proxy = profile.use_proxies,
          .request_epoch = request_epoch,
      };
      auto decoded = provider_->request(request);
      if (!decoded) {
        return RenderResult<GpuImage>::failure(*decoded.error);
      }
      if (request_epoch != current_epoch()) {
        return stale();
      }

      VideoFrame decoded_frame{
          .timestamp = time,
          .duration = sequence->frame_rate.frameTime(),
          .width = (*decoded.value)->width(),
          .height = (*decoded.value)->height(),
          .layout = PixelLayout::RgbaFloat32,
          .bit_depth = 32,
          .color = {},
          .field_order = "progressive",
          .sample_aspect_ratio = edit::Time(1, 1),
          .orientation_degrees = 0,
          .alpha_mode = AlphaMode::Premultiplied,
          .storage = *decoded.value,
      };
      auto uploaded = renderer_->upload(decoded_frame);
      if (!uploaded) {
        return RenderResult<GpuImage>::failure(*uploaded.error);
      }
      layers.push_back(GpuLayer{.image = std::move(*uploaded.value),
                                .transform = clip.transform,
                                .blend_mode = clip.blend_mode});
    }
  }

  if (request_epoch != current_epoch()) {
    return stale();
  }
  auto composited =
      renderer_->composite_timeline(layers, width, height, sequence->width, sequence->height, time,
                                    sequence->frame_rate.frameTime());
  if (request_epoch != current_epoch()) {
    return stale();
  }
  return composited;
}

} // namespace video_editor::render
