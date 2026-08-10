// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/timeline_editor.h"
#include "video_editor/render_engine/frame.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace video_editor::render {

enum class PreviewScale : std::uint8_t { Full, Half, Quarter };

struct PreviewProfile {
  PreviewScale scale{PreviewScale::Full};
  bool bypass_expensive_effects{false};
  bool use_proxies{true};
};

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
