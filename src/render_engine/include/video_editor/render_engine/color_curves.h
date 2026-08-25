// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace video_editor::render {

struct CurvePoint final {
  double x{0.0};
  double y{0.0};
};

struct ColorCurves final {
  std::vector<CurvePoint> red;
  std::vector<CurvePoint> green;
  std::vector<CurvePoint> blue;
  std::vector<CurvePoint> luma;
};

// Parses "x,y;x,y;..." with optional auto-extension of identity endpoints.
[[nodiscard]] std::optional<ColorCurves> parse_color_curves(const std::string& red,
                                                            const std::string& green,
                                                            const std::string& blue,
                                                            const std::string& luma);

[[nodiscard]] double evaluate_curve(const std::vector<CurvePoint>& points, double x) noexcept;

void apply_color_curves(float& red, float& green, float& blue,
                        const ColorCurves& curves) noexcept;

} // namespace video_editor::render
