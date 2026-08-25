// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/white_balance.h"

#include <algorithm>
#include <cmath>

namespace video_editor::render {

WhiteBalanceAdjustment white_balance_from_linear_rgb(const double red, const double green,
                                                     const double blue) noexcept {
  const double clamped_red = std::clamp(red, 0.0, 1.0);
  const double clamped_green = std::clamp(green, 0.0, 1.0);
  const double clamped_blue = std::clamp(blue, 0.0, 1.0);
  const double target = (clamped_red + clamped_green + clamped_blue) / 3.0;
  // Solve the inverse of apply_color's offsets for a neutral gray target.
  const double tint = 10.0 * (clamped_green - target);
  const double temperature = (5.0 * (clamped_blue - clamped_red)) - (0.5 * tint);
  return {.temperature = std::clamp(temperature, -1.0, 1.0),
          .tint = std::clamp(tint, -1.0, 1.0)};
}

} // namespace video_editor::render
