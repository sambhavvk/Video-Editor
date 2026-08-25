// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace video_editor::render {

struct Lut3D final {
  int size{0};
  std::array<float, 3> domain_min{{0.0F, 0.0F, 0.0F}};
  std::array<float, 3> domain_max{{1.0F, 1.0F, 1.0F}};
  // Adobe cube order: R fastest, then G, then B (index = r + g*size + b*size*size).
  std::vector<std::array<float, 3>> lattice;

  [[nodiscard]] std::array<float, 3> sample(float red, float green, float blue) const noexcept;
};

[[nodiscard]] std::optional<Lut3D> parse_cube_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<Lut3D> parse_cube_file(const std::filesystem::path& path);

// Returns a shared lattice, parsing once per path identity (path + mtime + size).
[[nodiscard]] const Lut3D* cached_lut_for_path(const std::filesystem::path& path) noexcept;

} // namespace video_editor::render
