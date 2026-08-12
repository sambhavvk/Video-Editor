// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/frame.h"

#include <algorithm>
#include <stdexcept>

namespace video_editor::render {

CpuFrame::CpuFrame(const int width, const int height)
    : width_(width), height_(height),
      pixels_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0.0F) {
  if (width <= 0 || height <= 0) {
    throw std::invalid_argument("frame dimensions must be positive");
  }
}

std::span<float, 4> CpuFrame::pixel(const int x, const int y) {
  if (x < 0 || x >= width_ || y < 0 || y >= height_) {
    throw std::out_of_range("pixel coordinates are outside the frame");
  }
  return std::span<float, 4>(
      pixels_.data() +
          ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
            static_cast<std::size_t>(x)) *
           4U),
      4U);
}

std::span<const float, 4> CpuFrame::pixel(const int x, const int y) const {
  if (x < 0 || x >= width_ || y < 0 || y >= height_) {
    throw std::out_of_range("pixel coordinates are outside the frame");
  }
  return std::span<const float, 4>(
      pixels_.data() +
          ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
            static_cast<std::size_t>(x)) *
           4U),
      4U);
}

void CpuFrame::clear(const float red, const float green, const float blue,
                     const float alpha) noexcept {
  for (std::size_t index = 0; index < pixels_.size(); index += 4U) {
    pixels_[index] = red * alpha;
    pixels_[index + 1U] = green * alpha;
    pixels_[index + 2U] = blue * alpha;
    pixels_[index + 3U] = alpha;
  }
}

} // namespace video_editor::render
