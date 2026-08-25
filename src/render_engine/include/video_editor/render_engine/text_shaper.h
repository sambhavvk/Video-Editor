// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"
#include "video_editor/render_engine/frame.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace video_editor::render {

enum class TextHorizontalAlignment : std::uint8_t { Left, Center, Right };

struct TextBlockMetrics final {
  int width{0};
  int height{0};
  int line_height{0};
  std::vector<int> line_widths;
};

// True when `font_family` maps to the bundled Noto Sans and shaping is available.
[[nodiscard]] bool font_family_uses_shaped_noto(std::string_view font_family) noexcept;

[[nodiscard]] TextBlockMetrics measure_text_block(std::string_view text, std::string_view font_family,
                                                  double font_size_px, bool bold, bool italic);

// Draws one UTF-8 line at the top-left cell origin `(x, y)`.
void draw_text_line(CpuFrame& frame, std::string_view line_text, std::string_view font_family, int x,
                    int y, double font_size_px, bool bold, bool italic, const edit::ColorRgba& color,
                    const edit::ColorRgba& outline_color = {0.0, 0.0, 0.0, 0.0},
                    double outline_width = 0.0);

} // namespace video_editor::render
