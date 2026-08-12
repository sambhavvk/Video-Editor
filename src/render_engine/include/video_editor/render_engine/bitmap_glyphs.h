// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"
#include "video_editor/render_engine/frame.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace video_editor::render {

// Deterministic 5x7 bitmap glyph set shared by the title rasterizer and the
// caption burn-in renderer. The same payload must produce the same pixels on
// every machine, so glyphs are bundled and never read from platform fonts.
//
// Each glyph row is a 5-bit mask, bit 4 (0b10000) leftmost. An empty array
// (all zero rows) represents a blank advance (for example, a space). Unknown
// codepoints map to `replacement_glyph()` so output stays deterministic.

constexpr int kGlyphColumns = 5;
constexpr int kGlyphRows = 7;
using Glyph = std::array<std::uint8_t, kGlyphRows>;

[[nodiscard]] Glyph replacement_glyph() noexcept;

// ASCII letters are folded to upper case before lookup. Supported punctuation:
// space, '.', '-', '_', '?'. Anything else returns `replacement_glyph()`.
[[nodiscard]] Glyph glyph_for_ascii(char32_t codepoint) noexcept;

[[nodiscard]] bool supported_glyph(char32_t codepoint) noexcept;

// UTF-8 decoder used by both title and caption rasterizers. Ill-formed
// sequences produce U+FFFD, matching the replacement-glyph contract.
[[nodiscard]] std::vector<char32_t> decode_utf8_with_replacement(std::string_view text);

// Draws a glyph into `frame` at `origin_x`/`origin_y` (top-left of the cell),
// scaled by `scale` (each glyph pixel becomes a `scale`x`scale` block). `bold`
// widens the glyph horizontally; `italic` shears it rightwards. Pixels are
// blended source-over into the existing frame contents using premultiplied
// alpha, so captions drawn over composited video preserve the underlying image.
void draw_glyph(CpuFrame& frame, const Glyph& glyph, int origin_x, int origin_y, int scale,
                const edit::ColorRgba& color, bool bold, bool italic) noexcept;

} // namespace video_editor::render
