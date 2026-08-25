// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/color_curves.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
#include <vector>

namespace video_editor::render {
namespace {

[[nodiscard]] bool parse_double(std::string_view token, double& value) {
  if (token.empty()) {
    return false;
  }
  const char* begin = token.data();
  const char* end = begin + token.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end && std::isfinite(value);
}

[[nodiscard]] std::optional<std::vector<CurvePoint>> parse_curve_channel(std::string_view encoded) {
  if (encoded.empty()) {
    return std::nullopt;
  }
  std::vector<CurvePoint> points;
  while (!encoded.empty()) {
    const auto separator = encoded.find(';');
    const auto segment = encoded.substr(0, separator);
    encoded = separator == std::string_view::npos ? std::string_view{} : encoded.substr(separator + 1);
    if (segment.empty()) {
      return std::nullopt;
    }
    const auto comma = segment.find(',');
    if (comma == std::string_view::npos) {
      return std::nullopt;
    }
    CurvePoint point;
    if (!parse_double(segment.substr(0, comma), point.x) ||
        !parse_double(segment.substr(comma + 1), point.y)) {
      return std::nullopt;
    }
    if (point.x < 0.0 || point.x > 1.0 || point.y < 0.0 || point.y > 1.0) {
      return std::nullopt;
    }
    points.push_back(point);
  }
  if (points.size() < 2U) {
    return std::nullopt;
  }
  std::sort(points.begin(), points.end(),
            [](const CurvePoint& left, const CurvePoint& right) { return left.x < right.x; });
  for (std::size_t index = 1; index < points.size(); ++index) {
    if (points[index].x <= points[index - 1].x) {
      return std::nullopt;
    }
  }
  if (points.front().x > 0.0) {
    points.insert(points.begin(), CurvePoint{0.0, points.front().y});
  }
  if (points.back().x < 1.0) {
    points.push_back(CurvePoint{1.0, points.back().y});
  }
  if (points.front().x != 0.0 || points.back().x != 1.0) {
    return std::nullopt;
  }
  return points;
}

} // namespace

std::optional<ColorCurves> parse_color_curves(const std::string& red, const std::string& green,
                                              const std::string& blue, const std::string& luma) {
  ColorCurves curves;
  if (auto parsed = parse_curve_channel(red)) {
    curves.red = std::move(*parsed);
  } else {
    return std::nullopt;
  }
  if (auto parsed = parse_curve_channel(green)) {
    curves.green = std::move(*parsed);
  } else {
    return std::nullopt;
  }
  if (auto parsed = parse_curve_channel(blue)) {
    curves.blue = std::move(*parsed);
  } else {
    return std::nullopt;
  }
  if (auto parsed = parse_curve_channel(luma)) {
    curves.luma = std::move(*parsed);
  } else {
    return std::nullopt;
  }
  return curves;
}

double evaluate_curve(const std::vector<CurvePoint>& points, const double x) noexcept {
  if (points.empty()) {
    return x;
  }
  if (x <= points.front().x) {
    return points.front().y;
  }
  if (x >= points.back().x) {
    return points.back().y;
  }
  const auto upper = std::upper_bound(
      points.begin(), points.end(), x,
      [](const double value, const CurvePoint& point) { return value < point.x; });
  const auto& right = *upper;
  const auto& left = *std::prev(upper);
  if (right.x <= left.x) {
    return left.y;
  }
  const double progress = (x - left.x) / (right.x - left.x);
  return left.y + ((right.y - left.y) * progress);
}

void apply_color_curves(float& red, float& green, float& blue,
                        const ColorCurves& curves) noexcept {
  red = static_cast<float>(evaluate_curve(curves.red, static_cast<double>(red)));
  green = static_cast<float>(evaluate_curve(curves.green, static_cast<double>(green)));
  blue = static_cast<float>(evaluate_curve(curves.blue, static_cast<double>(blue)));

  const double luma = (0.2126 * static_cast<double>(red)) + (0.7152 * static_cast<double>(green)) +
                      (0.0722 * static_cast<double>(blue));
  const double adjusted_luma = evaluate_curve(curves.luma, luma);
  if (luma > 0.0) {
    const double scale = adjusted_luma / luma;
    red = static_cast<float>(std::clamp(static_cast<double>(red) * scale, 0.0, 1.0));
    green = static_cast<float>(std::clamp(static_cast<double>(green) * scale, 0.0, 1.0));
    blue = static_cast<float>(std::clamp(static_cast<double>(blue) * scale, 0.0, 1.0));
  } else {
    const float mapped = static_cast<float>(adjusted_luma);
    red = mapped;
    green = mapped;
    blue = mapped;
  }
}

} // namespace video_editor::render
