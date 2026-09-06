// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/render_engine/frame.h"

#include <cstdint>
#include <vector>

namespace video_editor::render {

struct Rgba8Image {
  int width{0};
  int height{0};
  std::vector<std::uint8_t> pixels; // RGBA8888 row-major, size width*height*4
};

[[nodiscard]] Rgba8Image cpu_frame_to_rgba8(const CpuFrame& frame);

} // namespace video_editor::render
