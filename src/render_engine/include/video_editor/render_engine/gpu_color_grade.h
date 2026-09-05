// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/render_engine/color_curves.h"
#include "video_editor/render_engine/lut3d.h"

#include <cstdint>
#include <string>
#include <vector>

namespace video_editor::render {

struct GpuColorGradeOp final {
  enum class Kind : std::uint8_t { Lut, Curves };
  Kind kind{Kind::Lut};
  const Lut3D* lut{nullptr};
  std::string lut_cache_key;
  ColorCurves curves{};
  std::string curves_cache_key;
};

struct GpuColorGrade final {
  std::vector<GpuColorGradeOp> ops;

  [[nodiscard]] bool empty() const noexcept { return ops.empty(); }
};

} // namespace video_editor::render
