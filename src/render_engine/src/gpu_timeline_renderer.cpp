// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/gpu_timeline_renderer.h"

#include "video_editor/render_engine/cpu_renderer.h"

#include <algorithm>
#include <cmath>
#include <functional>
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

[[nodiscard]] RenderResult<GpuImage> stale() {
  return RenderResult<GpuImage>::failure(
      {.code = RenderErrorCode::StaleRequest,
       .message = "GPU timeline request belongs to a stale epoch"});
}

[[nodiscard]] RenderResult<GpuImage> unsupported_timeline(std::string message) {
  return RenderResult<GpuImage>::failure(
      {.code = RenderErrorCode::GpuUnsupportedTimeline, .message = std::move(message)});
}

[[nodiscard]] bool valid_clip_transform(const edit::Transform& transform,
                                        std::string& diagnostic) {
  if (!std::isfinite(transform.scale.x) || !std::isfinite(transform.scale.y) ||
      transform.scale.x == 0.0 || transform.scale.y == 0.0) {
    diagnostic = "GPU timeline preview requires finite, non-zero clip scale";
    return false;
  }
  return true;
}

[[nodiscard]] VideoFrame make_video_frame(const std::shared_ptr<CpuFrame>& frame,
                                          const edit::Time timestamp, const edit::Time duration) {
  return VideoFrame{
      .timestamp = timestamp,
      .duration = duration,
      .width = frame->width(),
      .height = frame->height(),
      .layout = PixelLayout::RgbaFloat32,
      .bit_depth = 32,
      .color = {},
      .field_order = "progressive",
      .sample_aspect_ratio = edit::Time(1, 1),
      .orientation_degrees = 0,
      .alpha_mode = AlphaMode::Premultiplied,
      .storage = frame,
  };
}

[[nodiscard]] RenderResult<GpuLayer>
build_gpu_layer(const edit::Clip& clip, const edit::Sequence& sequence, const edit::Time time,
                const PreviewProfile& profile, const std::uint64_t request_epoch, const int width,
                const int height, FrameProvider& provider,
                const std::function<std::uint64_t()>& current_epoch, GpuRenderer& renderer) {
  if (clip_has_unsupported_gpu_effects(clip.effects)) {
    return RenderResult<GpuLayer>::failure(
        {.code = RenderErrorCode::GpuUnsupportedTimeline,
         .message = "GPU timeline preview does not yet support enabled clip effects of this type"});
  }
  std::string diagnostic;
  if (!valid_clip_transform(clip.transform, diagnostic)) {
    return RenderResult<GpuLayer>::failure(
        {.code = RenderErrorCode::GpuUnsupportedTimeline, .message = std::move(diagnostic)});
  }
  if (clip.kind != edit::ClipKind::Video && clip.kind != edit::ClipKind::Title) {
    return RenderResult<GpuLayer>::failure(
        {.code = RenderErrorCode::GpuUnsupportedTimeline,
         .message = "GPU timeline preview does not yet support this clip kind"});
  }

  edit::Clip mutable_clip = clip;
  const edit::Time local_time = time - clip.timeline_range.start;
  std::shared_ptr<CpuFrame> pixels;
  if (clip.kind == edit::ClipKind::Title) {
    pixels = rasterize_title_frame(clip, sequence, profile);
    apply_clip_visual_effects(*pixels, mutable_clip, local_time, profile);
  } else {
    AssetFrameRequest request{
        .asset_id = clip.asset_id,
        .source_time = source_time_for(clip, time),
        .preferred_width = width,
        .preferred_height = height,
        .permit_proxy = profile.use_proxies,
        .request_epoch = request_epoch,
    };
    auto decoded = provider.request(request);
    if (!decoded) {
      return RenderResult<GpuLayer>::failure(*decoded.error);
    }
    if (request_epoch != current_epoch()) {
      return RenderResult<GpuLayer>::failure(
          {.code = RenderErrorCode::StaleRequest,
           .message = "GPU timeline request was superseded during decoding"});
    }
    pixels = std::make_shared<CpuFrame>(**decoded.value);
    apply_clip_visual_effects(*pixels, mutable_clip, local_time, profile);
  }

  const edit::Time frame_duration = sequence.frame_rate.frameTime();
  auto uploaded = renderer.upload(make_video_frame(pixels, time, frame_duration));
  if (!uploaded) {
    return RenderResult<GpuLayer>::failure(*uploaded.error);
  }
  return RenderResult<GpuLayer>::success(
      GpuLayer{.image = std::move(*uploaded.value),
               .transform = mutable_clip.transform,
               .blend_mode = clip.blend_mode});
}

[[nodiscard]] RenderResult<GpuImage>
composite_layers(GpuRenderer& renderer, const std::span<const GpuLayer> layers, const int width,
                 const int height, const std::uint32_t sequence_width,
                 const std::uint32_t sequence_height, const edit::Time timestamp,
                 const edit::Time duration, const GpuImage* background) {
  return renderer.composite_timeline(layers, width, height, sequence_width, sequence_height,
                                     timestamp, duration, background);
}

[[nodiscard]] RenderResult<GpuImage> upload_cpu_result(GpuRenderer& renderer,
                                                       const std::shared_ptr<CpuFrame>& frame,
                                                       const edit::Time timestamp,
                                                       const edit::Time duration) {
  auto uploaded = renderer.upload(make_video_frame(frame, timestamp, duration));
  if (!uploaded) {
    return RenderResult<GpuImage>::failure(*uploaded.error);
  }
  return RenderResult<GpuImage>::success(std::move(*uploaded.value));
}

[[nodiscard]] RenderResult<GpuImage>
render_transition_track(GpuRenderer& renderer, FrameProvider& provider,
                        const edit::Sequence& sequence, const ActiveTransitionInfo& transition,
                        const edit::Time time, const PreviewProfile& profile,
                        const std::uint64_t request_epoch, const int width, const int height,
                        const std::function<std::uint64_t()>& current_epoch,
                        const GpuImage* baseline) {
  const edit::Time frame_duration = sequence.frame_rate.frameTime();
  auto outgoing_layer =
      build_gpu_layer(*transition.outgoing, sequence, time, profile, request_epoch, width, height,
                      provider, current_epoch, renderer);
  if (!outgoing_layer) {
    return RenderResult<GpuImage>::failure(*outgoing_layer.error);
  }
  if (request_epoch != current_epoch()) {
    return stale();
  }
  auto incoming_layer =
      build_gpu_layer(*transition.incoming, sequence, time, profile, request_epoch, width, height,
                      provider, current_epoch, renderer);
  if (!incoming_layer) {
    return RenderResult<GpuImage>::failure(*incoming_layer.error);
  }
  if (request_epoch != current_epoch()) {
    return stale();
  }

  auto outgoing_image = composite_layers(renderer, std::span<const GpuLayer>(&*outgoing_layer.value, 1),
                                       width, height, sequence.width, sequence.height, time,
                                       frame_duration, baseline);
  if (!outgoing_image) {
    return RenderResult<GpuImage>::failure(*outgoing_image.error);
  }
  auto incoming_image = composite_layers(renderer, std::span<const GpuLayer>(&*incoming_layer.value, 1),
                                         width, height, sequence.width, sequence.height, time,
                                         frame_duration, baseline);
  if (!incoming_image) {
    return RenderResult<GpuImage>::failure(*incoming_image.error);
  }

  auto outgoing_cpu = renderer.download(*outgoing_image.value);
  if (!outgoing_cpu) {
    return RenderResult<GpuImage>::failure(*outgoing_cpu.error);
  }
  auto incoming_cpu = renderer.download(*incoming_image.value);
  if (!incoming_cpu) {
    return RenderResult<GpuImage>::failure(*incoming_cpu.error);
  }
  const auto outgoing_pixels =
      std::get<std::shared_ptr<const CpuFrame>>(outgoing_cpu.value->storage);
  const auto incoming_pixels =
      std::get<std::shared_ptr<const CpuFrame>>(incoming_cpu.value->storage);
  std::shared_ptr<CpuFrame> blended_pixels;
  const edit::Time cut_time = transition.incoming->timeline_range.start;
  if (transition.transition->kind == edit::TransitionKind::CrossDissolve) {
    blended_pixels = blend_frames(
        *outgoing_pixels, *incoming_pixels,
        cpu_timeline_saturate(cpu_timeline_time_ratio(time - transition.transition->range.start,
                                                      transition.transition->range.duration)));
  } else if (time < cut_time) {
    blended_pixels = blend_frames(
        *outgoing_pixels, *opaque_black_frame_like(*outgoing_pixels),
        cpu_timeline_saturate(cpu_timeline_time_ratio(
            time - transition.transition->range.start,
            cut_time - transition.transition->range.start)));
  } else {
    blended_pixels = blend_frames(
        *opaque_black_frame_like(*incoming_pixels), *incoming_pixels,
        cpu_timeline_saturate(
            cpu_timeline_time_ratio(time - cut_time, transition.transition->range.end() - cut_time)));
  }
  return upload_cpu_result(renderer, blended_pixels, time, frame_duration);
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
  const edit::Time frame_duration = sequence->frame_rate.frameTime();
  const auto current_epoch_fn = [this]() { return current_epoch(); };

  std::optional<GpuImage> accumulator;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Video || track.muted || !track.visible) {
      continue;
    }

    const auto transition = active_transition_for_track(*sequence, track, time);
    if (transition.has_value()) {
      auto rendered = render_transition_track(*renderer_, *provider_, *sequence, *transition, time,
                                            profile, request_epoch, width, height,
                                            current_epoch_fn,
                                            accumulator.has_value() ? &*accumulator : nullptr);
      if (request_epoch != current_epoch()) {
        return stale();
      }
      if (!rendered) {
        return rendered;
      }
      accumulator = std::move(*rendered.value);
      continue;
    }

    std::vector<GpuLayer> track_layers;
    for (const edit::Clip& clip : track.clips) {
      if (!clip.timeline_range.contains(time)) {
        continue;
      }
      if (clip_has_unsupported_gpu_effects(clip.effects)) {
        return unsupported_timeline(
            "GPU timeline preview does not yet support enabled clip effects of this type");
      }
      auto layer =
          build_gpu_layer(clip, *sequence, time, profile, request_epoch, width, height, *provider_,
                          current_epoch_fn, *renderer_);
      if (request_epoch != current_epoch()) {
        return stale();
      }
      if (!layer) {
        return RenderResult<GpuImage>::failure(*layer.error);
      }
      track_layers.push_back(std::move(*layer.value));
    }
    if (track_layers.empty()) {
      continue;
    }

    auto rendered =
        composite_layers(*renderer_, track_layers, width, height, sequence->width, sequence->height,
                         time, frame_duration,
                         accumulator.has_value() ? &*accumulator : nullptr);
    if (request_epoch != current_epoch()) {
      return stale();
    }
    if (!rendered) {
      return rendered;
    }
    accumulator = std::move(*rendered.value);
  }

  if (!accumulator.has_value()) {
    return composite_layers(*renderer_, {}, width, height, sequence->width, sequence->height, time,
                            frame_duration, nullptr);
  }
  return RenderResult<GpuImage>::success(std::move(*accumulator));
}

} // namespace video_editor::render
