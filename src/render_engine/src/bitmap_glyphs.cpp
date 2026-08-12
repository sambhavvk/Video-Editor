// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/bitmap_glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace video_editor::render {
namespace {

[[nodiscard]] float saturate(const double value) noexcept {
  return std::clamp(static_cast<float>(value), 0.0F, 1.0F);
}

// Premultiplied source-over blend for a single destination pixel. This mirrors
// the blend used by the title rasterizer in cpu_renderer.cpp so that captions
// drawn over composited video match the title compositing contract exactly.
void blend_source_over(const edit::ColorRgba& source, std::span<float, 4> destination) noexcept {
  const float source_alpha = saturate(source.alpha);
  const float destination_alpha = std::clamp(destination[3], 0.0F, 1.0F);
  const float source_channels[3] = {saturate(source.red), saturate(source.green),
                                    saturate(source.blue)};
  for (std::size_t channel = 0; channel < 3U; ++channel) {
    const float source_premultiplied = saturate(source_channels[channel] * source.alpha);
    const float source_straight =
        source_alpha > 0.0F
            ? std::clamp(static_cast<float>(source_channels[channel] / source.alpha), 0.0F, 1.0F)
            : 0.0F;
    // Normal blend mode only; captions and titles both use source-over.
    const float blended = source_straight;
    destination[channel] =
        std::clamp(((1.0F - source_alpha) * destination[channel]) +
                       ((1.0F - destination_alpha) * source_premultiplied) +
                       (source_alpha * destination_alpha * blended),
                   0.0F, 1.0F);
  }
  destination[3] = std::clamp(source_alpha + destination_alpha -
                                  (source_alpha * destination_alpha),
                              0.0F, 1.0F);
}

} // namespace

Glyph replacement_glyph() noexcept {
  return {0b11111U, 0b10001U, 0b01010U, 0b00100U, 0b01010U, 0b10001U, 0b11111U};
}

Glyph glyph_for_ascii(const char32_t codepoint) noexcept {
  const char32_t upper =
      codepoint >= U'a' && codepoint <= U'z' ? codepoint - (U'a' - U'A') : codepoint;
  switch (upper) {
  case U' ':
    return {};
  case U'.':
    return {0U, 0U, 0U, 0U, 0U, 0b01100U, 0b01100U};
  case U'-':
    return {0U, 0U, 0U, 0b11111U, 0U, 0U, 0U};
  case U'_':
    return {0U, 0U, 0U, 0U, 0U, 0U, 0b11111U};
  case U'?':
    return {0b01110U, 0b10001U, 0b00010U, 0b00100U, 0b00100U, 0U, 0b00100U};
  case U'0':
    return {0b01110U, 0b10001U, 0b10011U, 0b10101U, 0b11001U, 0b10001U, 0b01110U};
  case U'1':
    return {0b00100U, 0b01100U, 0b00100U, 0b00100U, 0b00100U, 0b00100U, 0b01110U};
  case U'2':
    return {0b01110U, 0b10001U, 0b00001U, 0b00010U, 0b00100U, 0b01000U, 0b11111U};
  case U'3':
    return {0b11110U, 0b00001U, 0b00001U, 0b01110U, 0b00001U, 0b00001U, 0b11110U};
  case U'4':
    return {0b00010U, 0b00110U, 0b01010U, 0b10010U, 0b11111U, 0b00010U, 0b00010U};
  case U'5':
    return {0b11111U, 0b10000U, 0b11110U, 0b00001U, 0b00001U, 0b10001U, 0b01110U};
  case U'6':
    return {0b00110U, 0b01000U, 0b10000U, 0b11110U, 0b10001U, 0b10001U, 0b01110U};
  case U'7':
    return {0b11111U, 0b00001U, 0b00010U, 0b00100U, 0b01000U, 0b01000U, 0b01000U};
  case U'8':
    return {0b01110U, 0b10001U, 0b10001U, 0b01110U, 0b10001U, 0b10001U, 0b01110U};
  case U'9':
    return {0b01110U, 0b10001U, 0b10001U, 0b01111U, 0b00001U, 0b00010U, 0b11100U};
  case U'A':
    return {0b01110U, 0b10001U, 0b10001U, 0b11111U, 0b10001U, 0b10001U, 0b10001U};
  case U'B':
    return {0b11110U, 0b10001U, 0b10001U, 0b11110U, 0b10001U, 0b10001U, 0b11110U};
  case U'C':
    return {0b01110U, 0b10001U, 0b10000U, 0b10000U, 0b10000U, 0b10001U, 0b01110U};
  case U'D':
    return {0b11100U, 0b10010U, 0b10001U, 0b10001U, 0b10001U, 0b10010U, 0b11100U};
  case U'E':
    return {0b11111U, 0b10000U, 0b10000U, 0b11110U, 0b10000U, 0b10000U, 0b11111U};
  case U'F':
    return {0b11111U, 0b10000U, 0b10000U, 0b11110U, 0b10000U, 0b10000U, 0b10000U};
  case U'G':
    return {0b01110U, 0b10001U, 0b10000U, 0b10111U, 0b10001U, 0b10001U, 0b01110U};
  case U'H':
    return {0b10001U, 0b10001U, 0b10001U, 0b11111U, 0b10001U, 0b10001U, 0b10001U};
  case U'I':
    return {0b01110U, 0b00100U, 0b00100U, 0b00100U, 0b00100U, 0b00100U, 0b01110U};
  case U'J':
    return {0b00001U, 0b00001U, 0b00001U, 0b00001U, 0b10001U, 0b10001U, 0b01110U};
  case U'K':
    return {0b10001U, 0b10010U, 0b10100U, 0b11000U, 0b10100U, 0b10010U, 0b10001U};
  case U'L':
    return {0b10000U, 0b10000U, 0b10000U, 0b10000U, 0b10000U, 0b10000U, 0b11111U};
  case U'M':
    return {0b10001U, 0b11011U, 0b10101U, 0b10101U, 0b10001U, 0b10001U, 0b10001U};
  case U'N':
    return {0b10001U, 0b11001U, 0b10101U, 0b10011U, 0b10001U, 0b10001U, 0b10001U};
  case U'O':
    return {0b01110U, 0b10001U, 0b10001U, 0b10001U, 0b10001U, 0b10001U, 0b01110U};
  case U'P':
    return {0b11110U, 0b10001U, 0b10001U, 0b11110U, 0b10000U, 0b10000U, 0b10000U};
  case U'Q':
    return {0b01110U, 0b10001U, 0b10001U, 0b10001U, 0b10101U, 0b10010U, 0b01101U};
  case U'R':
    return {0b11110U, 0b10001U, 0b10001U, 0b11110U, 0b10100U, 0b10010U, 0b10001U};
  case U'S':
    return {0b01111U, 0b10000U, 0b10000U, 0b01110U, 0b00001U, 0b00001U, 0b11110U};
  case U'T':
    return {0b11111U, 0b00100U, 0b00100U, 0b00100U, 0b00100U, 0b00100U, 0b00100U};
  case U'U':
    return {0b10001U, 0b10001U, 0b10001U, 0b10001U, 0b10001U, 0b10001U, 0b01110U};
  case U'V':
    return {0b10001U, 0b10001U, 0b10001U, 0b10001U, 0b10001U, 0b01010U, 0b00100U};
  case U'W':
    return {0b10001U, 0b10001U, 0b10001U, 0b10101U, 0b10101U, 0b10101U, 0b01010U};
  case U'X':
    return {0b10001U, 0b10001U, 0b01010U, 0b00100U, 0b01010U, 0b10001U, 0b10001U};
  case U'Y':
    return {0b10001U, 0b10001U, 0b01010U, 0b00100U, 0b00100U, 0b00100U, 0b00100U};
  case U'Z':
    return {0b11111U, 0b00001U, 0b00010U, 0b00100U, 0b01000U, 0b10000U, 0b11111U};
  default:
    return replacement_glyph();
  }
}

bool supported_glyph(const char32_t codepoint) noexcept {
  return (codepoint >= U'0' && codepoint <= U'9') || (codepoint >= U'A' && codepoint <= U'Z') ||
         (codepoint >= U'a' && codepoint <= U'z') || codepoint == U' ' || codepoint == U'.' ||
         codepoint == U'-' || codepoint == U'_' || codepoint == U'?';
}

std::vector<char32_t> decode_utf8_with_replacement(const std::string_view text) {
  std::vector<char32_t> codepoints;
  const auto append_replacement = [&codepoints]() { codepoints.push_back(U'\uFFFD'); };
  for (std::size_t index = 0; index < text.size();) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80U) {
      codepoints.push_back(static_cast<char32_t>(byte));
      ++index;
      continue;
    }
    std::size_t length = 0;
    char32_t codepoint = 0;
    if ((byte & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = static_cast<char32_t>(byte & 0x1FU);
    } else if ((byte & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = static_cast<char32_t>(byte & 0x0FU);
    } else if ((byte & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = static_cast<char32_t>(byte & 0x07U);
    } else {
      append_replacement();
      ++index;
      continue;
    }
    if (index + length > text.size()) {
      append_replacement();
      break;
    }
    bool valid = true;
    for (std::size_t continuation = 1; continuation < length; ++continuation) {
      const auto next = static_cast<unsigned char>(text[index + continuation]);
      if ((next & 0xC0U) != 0x80U) {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6U) | static_cast<char32_t>(next & 0x3FU);
    }
    if (!valid || (length == 2 && codepoint < 0x80U) || (length == 3 && codepoint < 0x800U) ||
        (length == 4 && (codepoint < 0x10000U || codepoint > 0x10FFFFU)) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      append_replacement();
      ++index;
      continue;
    }
    codepoints.push_back(codepoint);
    index += length;
  }
  return codepoints;
}

void draw_glyph(CpuFrame& frame, const Glyph& glyph, const int origin_x, const int origin_y,
                const int scale, const edit::ColorRgba& color, const bool bold,
                const bool italic) noexcept {
  for (int row = 0; row < kGlyphRows; ++row) {
    const int italic_offset = italic ? ((kGlyphRows - 1 - row) * scale) / 3 : 0;
    for (int column = 0; column < kGlyphColumns; ++column) {
      if ((glyph[static_cast<std::size_t>(row)] &
           static_cast<std::uint8_t>(1U << ((kGlyphColumns - 1) - column))) == 0U) {
        continue;
      }
      for (int scale_y = 0; scale_y < scale; ++scale_y) {
        const int y = origin_y + (row * scale) + scale_y;
        if (y < 0 || y >= frame.height()) {
          continue;
        }
        for (int scale_x = 0; scale_x < scale; ++scale_x) {
          const int x = origin_x + italic_offset + (column * scale) + scale_x;
          const int bold_steps = bold ? std::max(1, scale / 2) : 0;
          for (int weight = 0; weight <= bold_steps; ++weight) {
            const int weighted_x = x + weight;
            if (weighted_x < 0 || weighted_x >= frame.width()) {
              continue;
            }
            blend_source_over(color, frame.pixel(weighted_x, y));
          }
        }
      }
    }
  }
}

} // namespace video_editor::render
