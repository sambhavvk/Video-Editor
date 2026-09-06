// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/cpu_frame_encode.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace video_editor::render {
namespace {

[[nodiscard]] std::uint8_t encode_linear_to_rec709(const float linear) {
  const float clamped = std::max(0.0F, linear);
  const float encoded =
      clamped <= 0.0031308F ? clamped * 12.92F : (1.055F * std::pow(clamped, 1.0F / 2.4F)) - 0.055F;
  return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
}

} // namespace

Rgba8Image cpu_frame_to_rgba8(const CpuFrame& frame) {
  Rgba8Image image;
  image.width = frame.width();
  image.height = frame.height();
  if (image.width <= 0 || image.height <= 0) {
    return image;
  }

  const auto pixel_count =
      static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
  image.pixels.resize(pixel_count * 4U);

  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const auto pixel = frame.pixel(x, y);
      const float alpha = std::clamp(pixel[3], 0.0F, 1.0F);
      const float inverse_alpha = alpha > 0.0F ? 1.0F / alpha : 0.0F;
      const std::size_t offset =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
           static_cast<std::size_t>(x)) *
          4U;
      image.pixels[offset + 0U] = encode_linear_to_rec709(pixel[0] * inverse_alpha);
      image.pixels[offset + 1U] = encode_linear_to_rec709(pixel[1] * inverse_alpha);
      image.pixels[offset + 2U] = encode_linear_to_rec709(pixel[2] * inverse_alpha);
      image.pixels[offset + 3U] = static_cast<std::uint8_t>(std::lround(alpha * 255.0F));
    }
  }
  return image;
}

} // namespace video_editor::render
