// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/timeline_editor.h"
#include "video_editor/render_engine/frame.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::render {

enum class PreviewScale : std::uint8_t { Full, Half, Quarter };

struct PreviewProfile {
  PreviewScale scale{PreviewScale::Full};
  bool bypass_expensive_effects{false};
  bool use_proxies{true};
};

[[nodiscard]] inline int preview_output_dimension(const std::uint32_t value,
                                                  const PreviewScale scale) noexcept {
  const int divisor = scale == PreviewScale::Full ? 1 : scale == PreviewScale::Half ? 2 : 4;
  return value == 0U ? 1 : std::max(1, static_cast<int>(value) / divisor);
}

[[nodiscard]] inline std::uint64_t preview_graph_signature(const PreviewProfile& profile,
                                                           const std::uint64_t registry_generation) noexcept {
  std::uint64_t seed = static_cast<std::uint64_t>(static_cast<std::uint8_t>(profile.scale)) + 1U;
  seed = (seed * 0x9e3779b97f4a7c15ULL) + (profile.bypass_expensive_effects ? 1ULL : 0ULL);
  seed = (seed * 0x9e3779b97f4a7c15ULL) + (profile.use_proxies ? 1ULL : 0ULL);
  seed = (seed * 0x9e3779b97f4a7c15ULL) + registry_generation;
  return seed;
}

enum class RenderErrorCode : std::uint8_t {
  InvalidSnapshot,
  InvalidTime,
  AssetUnavailable,
  ProviderFailure,
  StaleRequest,
  GpuUnavailable,
  GpuInvalidFrame,
  GpuUploadFailed,
  GpuRenderFailed,
  GpuDownloadFailed,
  GpuPresentationUnavailable,
  GpuPresentFailed,
  GpuDeviceLost,
  GpuUnsupportedTimeline,
};

struct RenderError {
  RenderErrorCode code{RenderErrorCode::ProviderFailure};
  std::string message;
};

template <typename T> struct RenderResult {
  std::optional<T> value;
  std::optional<RenderError> error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return value.has_value();
  }
  [[nodiscard]] static RenderResult success(T result) {
    return {.value = std::move(result), .error = std::nullopt};
  }
  [[nodiscard]] static RenderResult failure(RenderError failure) {
    return {.value = std::nullopt, .error = std::move(failure)};
  }
};

struct AssetFrameRequest {
  edit::EntityId asset_id;
  edit::Time source_time;
  int preferred_width{0};
  int preferred_height{0};
  bool permit_proxy{true};
  std::uint64_t request_epoch{0};
};

class FrameProvider {
public:
  virtual ~FrameProvider() = default;
  [[nodiscard]] virtual RenderResult<std::shared_ptr<const CpuFrame>>
  request(const AssetFrameRequest& request) = 0;
};

struct ActiveTransitionInfo final {
  const edit::Transition* transition{nullptr};
  const edit::Clip* outgoing{nullptr};
  const edit::Clip* incoming{nullptr};
};

[[nodiscard]] std::shared_ptr<CpuFrame> rasterize_title_frame(const edit::Clip& clip,
                                                              const edit::Sequence& sequence,
                                                              const PreviewProfile& profile);

void apply_clip_visual_effects(CpuFrame& frame, edit::Clip& clip, edit::Time local_time,
                               const PreviewProfile& profile);

[[nodiscard]] std::optional<ActiveTransitionInfo>
active_transition_for_track(const edit::Sequence& sequence, const edit::Track& track,
                            edit::Time time);

[[nodiscard]] std::shared_ptr<CpuFrame> blend_frames(const CpuFrame& left, const CpuFrame& right,
                                                     float factor);

[[nodiscard]] std::shared_ptr<CpuFrame> opaque_black_frame_like(const CpuFrame& source);

[[nodiscard]] float cpu_timeline_saturate(double value) noexcept;

[[nodiscard]] double cpu_timeline_time_ratio(edit::Time numerator, edit::Time denominator);

[[nodiscard]] bool clip_has_unsupported_gpu_effects(const std::vector<edit::Effect>& effects);

void composite_clip_onto_frame(const CpuFrame& source, CpuFrame& destination, const edit::Clip& clip,
                               std::uint32_t sequence_width, std::uint32_t sequence_height);

void composite_blend_frame(CpuFrame& destination, const CpuFrame& source,
                           edit::BlendMode blend_mode);

class CpuRenderer final {
public:
  explicit CpuRenderer(std::shared_ptr<FrameProvider> provider);

  void begin_epoch(std::uint64_t request_epoch) noexcept;
  [[nodiscard]] std::uint64_t current_epoch() const noexcept;
  [[nodiscard]] RenderResult<VideoFrame> request_frame(const edit::TimelineSnapshot& snapshot,
                                                       edit::Time time,
                                                       const PreviewProfile& profile,
                                                       std::uint64_t request_epoch) const;

private:
  std::shared_ptr<FrameProvider> provider_;
  std::atomic<std::uint64_t> epoch_{0};
};

} // namespace video_editor::render
