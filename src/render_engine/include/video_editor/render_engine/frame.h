// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/time.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace video_editor::render {

enum class PixelLayout : std::uint8_t { RgbaFloat32, Rgba8, Yuv420Planar, NativeGpu };
enum class AlphaMode : std::uint8_t { Opaque, Straight, Premultiplied };

struct FrameColor {
  std::string primaries{"bt709"};
  std::string transfer{"linear"};
  std::string matrix{"rgb"};
  std::string range{"full"};
  std::string chroma_location{"unspecified"};
};

struct NativeGpuHandle {
  enum class Api : std::uint8_t { D3D11, Vulkan };
  Api api{Api::Vulkan};
  std::uintptr_t resource{0};
  std::uintptr_t synchronization{0};
};

class CpuFrame final {
public:
  CpuFrame() = default;
  CpuFrame(int width, int height);

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] std::span<float> pixels() noexcept { return pixels_; }
  [[nodiscard]] std::span<const float> pixels() const noexcept { return pixels_; }
  [[nodiscard]] std::span<float, 4> pixel(int x, int y);
  [[nodiscard]] std::span<const float, 4> pixel(int x, int y) const;
  void clear(float red, float green, float blue, float alpha = 1.0F) noexcept;

private:
  int width_{0};
  int height_{0};
  std::vector<float> pixels_;
};

struct VideoFrame {
  edit::Time timestamp{};
  edit::Time duration{};
  int width{0};
  int height{0};
  PixelLayout layout{PixelLayout::RgbaFloat32};
  int bit_depth{32};
  FrameColor color{};
  std::string field_order{"progressive"};
  edit::Time sample_aspect_ratio{1, 1};
  int orientation_degrees{0};
  AlphaMode alpha_mode{AlphaMode::Premultiplied};
  std::variant<std::shared_ptr<const CpuFrame>, NativeGpuHandle> storage;
};

} // namespace video_editor::render

