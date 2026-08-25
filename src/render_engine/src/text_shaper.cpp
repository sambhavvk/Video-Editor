// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/text_shaper.h"

#include "video_editor/render_engine/bitmap_glyphs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <hb.h>
#include <hb-ft.h>

namespace video_editor::render {
namespace {

[[nodiscard]] float saturate(const double value) noexcept {
  return std::clamp(static_cast<float>(value), 0.0F, 1.0F);
}

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
    destination[channel] =
        std::clamp(((1.0F - source_alpha) * destination[channel]) +
                       ((1.0F - destination_alpha) * source_premultiplied) +
                       (source_alpha * destination_alpha * source_straight),
                   0.0F, 1.0F);
  }
  destination[3] = std::clamp(source_alpha + destination_alpha -
                                  (source_alpha * destination_alpha),
                              0.0F, 1.0F);
}

void blend_coverage(CpuFrame& frame, const int x, const int y, const float coverage,
                    const edit::ColorRgba& color) noexcept {
  if (coverage <= 0.0F || x < 0 || y < 0 || x >= frame.width() || y >= frame.height()) {
    return;
  }
  edit::ColorRgba blended{color.red, color.green, color.blue, color.alpha * coverage};
  blend_source_over(blended, frame.pixel(x, y));
}

[[nodiscard]] bool iequals_ascii(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    auto left_char = static_cast<unsigned char>(left[index]);
    auto right_char = static_cast<unsigned char>(right[index]);
    if (left_char >= 'A' && left_char <= 'Z') {
      left_char = static_cast<unsigned char>(left_char - ('A' - 'a'));
    }
    if (right_char >= 'A' && right_char <= 'Z') {
      right_char = static_cast<unsigned char>(right_char - ('A' - 'a'));
    }
    if (left_char != right_char) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] int bitmap_scale(const double font_size_px) noexcept {
  return std::max(1, static_cast<int>(std::lround(font_size_px / 8.0)));
}

struct ShapedFontState final {
  bool available{false};
  FT_Library library{};
  FT_Face face{};
  hb_font_t* font{nullptr};
  hb_buffer_t* buffer{nullptr};
};

ShapedFontState& shaped_font_state() {
  static ShapedFontState state;
  static bool attempted = false;
  if (!attempted) {
    attempted = true;
#ifndef VIDEO_EDITOR_NOTO_SANS_TTF
#error "VIDEO_EDITOR_NOTO_SANS_TTF must be defined by CMake"
#endif
    if (FT_Init_FreeType(&state.library) != 0) {
      return state;
    }
    if (FT_New_Face(state.library, VIDEO_EDITOR_NOTO_SANS_TTF, 0, &state.face) != 0) {
      FT_Done_FreeType(state.library);
      state.library = nullptr;
      return state;
    }
    state.font = hb_ft_font_create(state.face, nullptr);
    state.buffer = hb_buffer_create();
    if (state.font == nullptr || state.buffer == nullptr) {
      if (state.buffer != nullptr) {
        hb_buffer_destroy(state.buffer);
        state.buffer = nullptr;
      }
      if (state.font != nullptr) {
        hb_font_destroy(state.font);
        state.font = nullptr;
      }
      FT_Done_Face(state.face);
      state.face = nullptr;
      FT_Done_FreeType(state.library);
      state.library = nullptr;
      return state;
    }
    state.available = true;
  }
  return state;
}

[[nodiscard]] bool shaped_noto_available() noexcept {
  return shaped_font_state().available;
}

[[nodiscard]] bool maps_to_noto(std::string_view font_family) noexcept {
  if (font_family.empty()) {
    return true;
  }
  return iequals_ascii(font_family, "sans-serif") || iequals_ascii(font_family, "noto sans") ||
         iequals_ascii(font_family, "notosans");
}

[[nodiscard]] bool use_shaped_path(std::string_view font_family) noexcept {
  return maps_to_noto(font_family) && shaped_noto_available();
}

[[nodiscard]] int shaped_pixel_size(const double font_size_px) noexcept {
  return std::max(1, static_cast<int>(std::lround(font_size_px)));
}

void apply_face_style(FT_Face face, const int pixel_size, const bool bold, const bool italic) {
  FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size));
  if (italic) {
    FT_Matrix matrix{1 << 16, static_cast<FT_Long>(0.2 * (1 << 16)), 0, 1 << 16};
    FT_Set_Transform(face, &matrix, nullptr);
  } else {
    FT_Set_Transform(face, nullptr, nullptr);
  }
  (void)bold;
}

[[nodiscard]] int shaped_ascender(const FT_Face face) noexcept {
  return static_cast<int>(face->size->metrics.ascender / 64);
}

[[nodiscard]] int shaped_line_height(const FT_Face face) noexcept {
  return std::max(1, static_cast<int>(face->size->metrics.height / 64));
}

void draw_bitmap_glyph(CpuFrame& frame, const FT_Bitmap& bitmap, const int left, const int top,
                       const edit::ColorRgba& color, const int outline_pixels,
                       const edit::ColorRgba& outline_color, const bool bold,
                       const bool italic) noexcept {
  if (bitmap.width == 0U || bitmap.rows == 0U || bitmap.buffer == nullptr) {
    return;
  }
  for (unsigned row = 0; row < bitmap.rows; ++row) {
    for (unsigned column = 0; column < bitmap.width; ++column) {
      const auto coverage_byte =
          bitmap.buffer[static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.pitch) +
                        column];
      if (coverage_byte == 0U) {
        continue;
      }
      const float coverage = static_cast<float>(coverage_byte) / 255.0F;
      const int pixel_x = left + static_cast<int>(column);
      const int pixel_y = top + static_cast<int>(row);
      if (outline_pixels > 0 && outline_color.alpha > 0.0) {
        for (int offset_y = -outline_pixels; offset_y <= outline_pixels; ++offset_y) {
          for (int offset_x = -outline_pixels; offset_x <= outline_pixels; ++offset_x) {
            const auto squared_distance =
                static_cast<long long>(offset_x) * offset_x +
                static_cast<long long>(offset_y) * offset_y;
            const auto squared_radius =
                static_cast<long long>(outline_pixels) * outline_pixels;
            if (squared_distance > squared_radius) {
              continue;
            }
            blend_coverage(frame, pixel_x + offset_x, pixel_y + offset_y, coverage, outline_color);
          }
        }
      }
      blend_coverage(frame, pixel_x, pixel_y, coverage, color);
      if (bold) {
        const int bold_steps = std::max(1, outline_pixels > 0 ? 1 : 1);
        for (int weight = 1; weight <= bold_steps; ++weight) {
          blend_coverage(frame, pixel_x + weight, pixel_y, coverage, color);
        }
      }
      if (italic) {
        blend_coverage(frame, pixel_x + 1, pixel_y, coverage, color);
      }
    }
  }
}

void draw_shaped_line(CpuFrame& frame, std::string_view line_text, const int x, const int y,
                      const int pixel_size, const bool bold, const bool italic,
                      const edit::ColorRgba& color, const int outline_pixels,
                      const edit::ColorRgba& outline_color) {
  ShapedFontState& state = shaped_font_state();
  if (!state.available) {
    return;
  }
  apply_face_style(state.face, pixel_size, bold, italic);
  const int ascender = shaped_ascender(state.face);
  const int baseline_y = y + ascender;

  hb_buffer_clear_contents(state.buffer);
  hb_buffer_add_utf8(state.buffer, line_text.data(), static_cast<int>(line_text.size()), 0,
                      static_cast<int>(line_text.size()));
  hb_buffer_guess_segment_properties(state.buffer);
  hb_shape(state.font, state.buffer, nullptr, 0);

  const unsigned glyph_count = hb_buffer_get_length(state.buffer);
  if (glyph_count == 0U) {
    return;
  }
  hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(state.buffer, nullptr);
  hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(state.buffer, nullptr);

  int pen_x = x;
  const int embolden_strength = bold ? std::max(1, pixel_size / 24) : 0;
  for (unsigned index = 0; index < glyph_count; ++index) {
    const FT_UInt glyph_index = infos[index].codepoint;
    if (glyph_index == 0U) {
      pen_x += positions[index].x_advance / 64;
      continue;
    }
    if (FT_Load_Glyph(state.face, glyph_index, FT_LOAD_DEFAULT) != 0) {
      pen_x += positions[index].x_advance / 64;
      continue;
    }
    FT_GlyphSlot slot = state.face->glyph;
    if (bold && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
      FT_Outline_Embolden(&slot->outline, static_cast<FT_Pos>(embolden_strength * 64));
    }
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
      pen_x += positions[index].x_advance / 64;
      continue;
    }

    const int glyph_left =
        pen_x + (positions[index].x_offset / 64) + slot->bitmap_left;
    const int glyph_top = baseline_y + (positions[index].y_offset / 64) - slot->bitmap_top;
    draw_bitmap_glyph(frame, slot->bitmap, glyph_left, glyph_top, color, outline_pixels,
                      outline_color, bold, italic);
    pen_x += positions[index].x_advance / 64;
  }
}

[[nodiscard]] int measure_shaped_line_width(std::string_view line_text, const int pixel_size,
                                            const bool bold, const bool italic) {
  ShapedFontState& state = shaped_font_state();
  if (!state.available) {
    return 0;
  }
  apply_face_style(state.face, pixel_size, bold, italic);
  hb_buffer_clear_contents(state.buffer);
  hb_buffer_add_utf8(state.buffer, line_text.data(), static_cast<int>(line_text.size()), 0,
                      static_cast<int>(line_text.size()));
  hb_buffer_guess_segment_properties(state.buffer);
  hb_shape(state.font, state.buffer, nullptr, 0);
  const unsigned glyph_count = hb_buffer_get_length(state.buffer);
  if (glyph_count == 0U) {
    return 0;
  }
  hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(state.buffer, nullptr);
  int width = 0;
  for (unsigned index = 0; index < glyph_count; ++index) {
    width += positions[index].x_advance / 64;
  }
  if (italic) {
    width += std::max(1, pixel_size / 4);
  }
  if (bold) {
    width += std::max(1, pixel_size / 24);
  }
  return width;
}

[[nodiscard]] std::vector<std::string> split_utf8_lines(std::string_view text) {
  std::vector<std::string> lines(1);
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\n') {
      lines.emplace_back();
      continue;
    }
    lines.back().push_back(text[index]);
  }
  if (lines.empty()) {
    lines.emplace_back();
  }
  return lines;
}

[[nodiscard]] TextBlockMetrics measure_bitmap_block(std::string_view text, const double font_size_px,
                                                    const bool /*bold*/, const bool italic) {
  const int scale = bitmap_scale(font_size_px);
  const int cell_width = 6 * scale;
  const int cell_height = 8 * scale;
  const int italic_padding = italic ? std::max(1, (2 * scale)) : 0;
  const auto lines = split_utf8_lines(text);
  TextBlockMetrics metrics;
  metrics.line_height = cell_height;
  for (const auto& line : lines) {
    const auto codepoints = decode_utf8_with_replacement(line);
    const int line_width = static_cast<int>(codepoints.size()) * cell_width + italic_padding;
    metrics.line_widths.push_back(line_width);
    metrics.width = std::max(metrics.width, line_width);
  }
  metrics.height = static_cast<int>(lines.size()) * cell_height;
  return metrics;
}

void draw_bitmap_line(CpuFrame& frame, std::string_view line_text, const int x, const int y,
                      const double font_size_px, const bool bold, const bool italic,
                      const edit::ColorRgba& color, const int outline_pixels,
                      const edit::ColorRgba& outline_color) {
  const int scale = bitmap_scale(font_size_px);
  const int cell_width = 6 * scale;
  const auto codepoints = decode_utf8_with_replacement(line_text);
  int glyph_x = x;
  for (const char32_t codepoint : codepoints) {
    const Glyph glyph = supported_glyph(codepoint) ? glyph_for_ascii(codepoint) : replacement_glyph();
    if (outline_pixels > 0 && outline_color.alpha > 0.0) {
      for (int offset_y = -outline_pixels; offset_y <= outline_pixels; ++offset_y) {
        for (int offset_x = -outline_pixels; offset_x <= outline_pixels; ++offset_x) {
          const auto squared_distance =
              static_cast<long long>(offset_x) * offset_x +
              static_cast<long long>(offset_y) * offset_y;
          const auto squared_radius =
              static_cast<long long>(outline_pixels) * outline_pixels;
          if (squared_distance > squared_radius) {
            continue;
          }
          draw_glyph(frame, glyph, glyph_x + offset_x, y + offset_y, scale, outline_color, bold,
                     italic);
        }
      }
    }
    draw_glyph(frame, glyph, glyph_x, y, scale, color, bold, italic);
    glyph_x += cell_width;
  }
}

} // namespace

bool font_family_uses_shaped_noto(const std::string_view font_family) noexcept {
  return use_shaped_path(font_family);
}

TextBlockMetrics measure_text_block(const std::string_view text, const std::string_view font_family,
                                    const double font_size_px, const bool bold, const bool italic) {
  const auto lines = split_utf8_lines(text);
  TextBlockMetrics metrics;
  if (use_shaped_path(font_family)) {
    const int pixel_size = shaped_pixel_size(font_size_px);
    apply_face_style(shaped_font_state().face, pixel_size, bold, italic);
    metrics.line_height = shaped_line_height(shaped_font_state().face);
    for (const auto& line : lines) {
      const int line_width = measure_shaped_line_width(line, pixel_size, bold, italic);
      metrics.line_widths.push_back(line_width);
      metrics.width = std::max(metrics.width, line_width);
    }
    metrics.height = static_cast<int>(lines.size()) * metrics.line_height;
    return metrics;
  }
  return measure_bitmap_block(text, font_size_px, bold, italic);
}

void draw_text_line(CpuFrame& frame, const std::string_view line_text,
                    const std::string_view font_family, const int x, const int y,
                    const double font_size_px, const bool bold, const bool italic,
                    const edit::ColorRgba& color, const edit::ColorRgba& outline_color,
                    const double outline_width) {
  const int outline_pixels =
      std::isfinite(outline_width)
          ? static_cast<int>(std::ceil(std::clamp(outline_width, 0.0, 1024.0)))
          : 0;
  if (use_shaped_path(font_family)) {
    draw_shaped_line(frame, line_text, x, y, shaped_pixel_size(font_size_px), bold, italic, color,
                     outline_pixels, outline_color);
    return;
  }
  draw_bitmap_line(frame, line_text, x, y, font_size_px, bold, italic, color, outline_pixels,
                   outline_color);
}

} // namespace video_editor::render
