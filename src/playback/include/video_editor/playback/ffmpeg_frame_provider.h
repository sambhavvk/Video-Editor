// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/time.h"
#include "video_editor/playback/asset_registry.h"
#include "video_editor/render_engine/cpu_renderer.h"

#include <cstdint>
#include <memory>

namespace video_editor::playback {

struct DecodedAssetFrame final {
  std::shared_ptr<const render::CpuFrame> pixels;
  // Exact source-relative, half-open presentation interval [start, end).
  edit::TimeRange presentation;
  bool used_proxy{false};
  int video_stream_index{-1};
};

struct PlaybackStatistics final {
  std::uint64_t sessions_opened{0};
  std::uint64_t sessions_reopened{0};
  std::uint64_t seeks{0};
  std::uint64_t sequential_requests{0};
  std::uint64_t cached_frame_requests{0};
  std::uint64_t decoded_frames{0};
};

// FFmpeg-backed, CPU-only frame provider. One provider serializes access to its
// persistent decoder sessions; begin_epoch() remains lock-free so it can cancel
// a blocked or long-running request from another thread.
class FfmpegFrameProvider final : public render::FrameProvider {
public:
  explicit FfmpegFrameProvider(std::shared_ptr<AssetRegistry> registry);
  ~FfmpegFrameProvider() override;

  FfmpegFrameProvider(const FfmpegFrameProvider&) = delete;
  FfmpegFrameProvider& operator=(const FfmpegFrameProvider&) = delete;

  void begin_epoch(std::uint64_t request_epoch) noexcept;
  [[nodiscard]] std::uint64_t current_epoch() const noexcept;

  [[nodiscard]] render::RenderResult<std::shared_ptr<const render::CpuFrame>>
  request(const render::AssetFrameRequest& request) override;

  [[nodiscard]] render::RenderResult<DecodedAssetFrame>
  request_with_timing(const render::AssetFrameRequest& request);

  // Explicit invalidation is useful after media relinking or replacement.
  void invalidate(const edit::EntityId& asset_id);
  void clear_sessions();
  [[nodiscard]] PlaybackStatistics statistics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::playback
