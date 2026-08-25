// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace video_editor::render {

struct WhiteBalanceAdjustment final {
  double temperature{0.0};
  double tint{0.0};
};

// Inverts the canonical CPU white-balance offsets used by apply_color:
//   red   += temperature * 0.1 + tint * 0.05;
//   green -= tint * 0.1;
//   blue  -= temperature * 0.1 + tint * 0.05;
// Neutral linear gray samples map to approximately (0, 0).
[[nodiscard]] WhiteBalanceAdjustment white_balance_from_linear_rgb(double red, double green,
                                                                   double blue) noexcept;

} // namespace video_editor::render
