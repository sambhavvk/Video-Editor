// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/render_engine/gpu_backend.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace video_editor::render {

// Decodes only the video clips active at the requested timeline time and
// hands their individual images to GpuRenderer for transform/composition.
// This deliberately does not run CpuRenderer first.
class GpuTimelineRenderer final {
public:
  GpuTimelineRenderer(std::shared_ptr<FrameProvider> provider,
                      std::shared_ptr<GpuRenderer> renderer);

  void begin_epoch(std::uint64_t request_epoch) noexcept;
  [[nodiscard]] std::uint64_t current_epoch() const noexcept;

  [[nodiscard]] RenderResult<GpuImage> request_frame(const edit::TimelineSnapshot& snapshot,
                                                     edit::Time time, const PreviewProfile& profile,
                                                     std::uint64_t request_epoch) const;

private:
  std::shared_ptr<FrameProvider> provider_;
  std::shared_ptr<GpuRenderer> renderer_;
  std::atomic<std::uint64_t> epoch_{0};
};

} // namespace video_editor::render
