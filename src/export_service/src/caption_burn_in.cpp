// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/caption_burn_in.h"

#include "video_editor/caption_service/caption_service.h"
#include "video_editor/render_engine/bitmap_glyphs.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace video_editor::export_service {
namespace {

void blend_source_over(const edit::ColorRgba& source, std::span<float, 4> destination) noexcept {
  const float source_alpha = std::clamp(static_cast<float>(source.alpha), 0.0F, 1.0F);
  const float destination_alpha = std::clamp(destination[3], 0.0F, 1.0F);
  const float channels[3] = {std::clamp(static_cast<float>(source.red), 0.0F, 1.0F),
                             std::clamp(static_cast<float>(source.green), 0.0F, 1.0F),
                             std::clamp(static_cast<float>(source.blue), 0.0F, 1.0F)};
  for (std::size_t channel = 0; channel < 3U; ++channel) {
    const float premultiplied = std::clamp(channels[channel] * source_alpha, 0.0F, 1.0F);
    destination[channel] = std::clamp((1.0F - source_alpha) * destination[channel] +
                                          (1.0F - destination_alpha) * premultiplied +
                                          source_alpha * destination_alpha * channels[channel],
                                      0.0F, 1.0F);
  }
  destination[3] =
      std::clamp(source_alpha + destination_alpha - source_alpha * destination_alpha, 0.0F, 1.0F);
}

void fill_background_rect(render::CpuFrame& frame, int left, int top, int right, int bottom,
                          const edit::ColorRgba& color) noexcept {
  for (int y = std::max(0, top); y < std::min(frame.height(), bottom); ++y) {
    for (int x = std::max(0, left); x < std::min(frame.width(), right); ++x) {
      blend_source_over(color, frame.pixel(x, y));
    }
  }
}

[[nodiscard]] int safe_margin_pixels(const double safe_margin, const int extent) noexcept {
  if (!std::isfinite(safe_margin)) {
    return 0;
  }
  return std::clamp(static_cast<int>(std::lround(std::clamp(safe_margin, 0.0, 0.5) * extent)), 0,
                    extent / 2);
}

[[nodiscard]] int aligned_left(const edit::CaptionAlignment alignment, const int safe_left,
                               const int safe_right, const int content_width) noexcept {
  switch (alignment) {
  case edit::CaptionAlignment::Left:
    return safe_left;
  case edit::CaptionAlignment::Right:
    return safe_right - content_width;
  case edit::CaptionAlignment::Center:
    return safe_left + (safe_right - safe_left - content_width) / 2;
  }
  return safe_left + (safe_right - safe_left - content_width) / 2;
}

[[nodiscard]] bool uses_legacy_default_position(const edit::CaptionStyle& style) noexcept {
  // These defaults were introduced with the canonical fields. Keep the
  // original frame-relative bottom anchor for old snapshots and callers that
  // construct a default CaptionStyle, while allowing any edited value to use
  // normalized safe-area placement.
  return std::abs(style.vertical_position - 0.9) <= 1.0e-12 &&
         std::abs(style.safe_margin - 0.05) <= 1.0e-12;
}

void draw_glyph_with_outline(render::CpuFrame& frame, const render::Glyph& glyph,
                             const int origin_x, const int origin_y, const int scale,
                             const edit::CaptionStyle& style, const int outline_pixels) noexcept {
  if (outline_pixels > 0 && style.outline_color.alpha > 0.0) {
    for (int offset_y = -outline_pixels; offset_y <= outline_pixels; ++offset_y) {
      for (int offset_x = -outline_pixels; offset_x <= outline_pixels; ++offset_x) {
        const auto squared_distance = static_cast<long long>(offset_x) * offset_x +
                                      static_cast<long long>(offset_y) * offset_y;
        const auto squared_radius = static_cast<long long>(outline_pixels) * outline_pixels;
        if (squared_distance > squared_radius) {
          continue;
        }
        render::draw_glyph(frame, glyph, origin_x + offset_x, origin_y + offset_y, scale,
                           style.outline_color, style.bold, style.italic);
      }
    }
  }
  render::draw_glyph(frame, glyph, origin_x, origin_y, scale, style.text_color, style.bold,
                     style.italic);
}

} // namespace

std::optional<CaptionBurnInError> draw_caption_text(render::CpuFrame& frame,
                                                    const edit::CaptionStyle& style,
                                                    const std::string_view text,
                                                    const int bottom_margin_pixels) {
  if (text.empty()) {
    return CaptionBurnInError::EmptyText;
  }
  if (style.font_size <= 0.0 || !std::isfinite(style.font_size)) {
    return CaptionBurnInError::InvalidFontSize;
  }

  const int scale = std::max(1, static_cast<int>(std::lround(style.font_size / 8.0)));
  const int cell_width = 6 * scale;
  const int cell_height = 8 * scale;
  if (frame.width() < cell_width || frame.height() < cell_height) {
    return CaptionBurnInError::FrameTooSmall;
  }

  const auto codepoints = render::decode_utf8_with_replacement(text);
  std::vector<std::vector<char32_t>> lines(1);
  for (const char32_t codepoint : codepoints) {
    if (codepoint == U'\n') {
      lines.emplace_back();
    } else {
      lines.back().push_back(codepoint);
    }
  }

  int total_width = 0;
  for (const auto& line : lines) {
    if (line.size() > static_cast<std::size_t>(std::numeric_limits<int>::max() / cell_width)) {
      return CaptionBurnInError::FrameTooSmall;
    }
    total_width = std::max(total_width, static_cast<int>(line.size()) * cell_width);
  }
  if (lines.size() > static_cast<std::size_t>(std::numeric_limits<int>::max() / cell_height)) {
    return CaptionBurnInError::FrameTooSmall;
  }
  const int total_height = static_cast<int>(lines.size()) * cell_height;
  const int safe_left = safe_margin_pixels(style.safe_margin, frame.width());
  const int safe_top = safe_margin_pixels(style.safe_margin, frame.height());
  const int safe_right = frame.width() - safe_left;
  const int safe_bottom = frame.height() - safe_top;
  const int content_left = aligned_left(style.alignment, safe_left, safe_right, total_width);

  // `bottom_margin_pixels` remains part of this low-level API for callers from
  // before normalized caption positioning existed. Edited canonical positions
  // take precedence; the legacy value is retained for invalid positions and
  // the canonical defaults so old snapshots render identically.
  const double normalized_position = std::isfinite(style.vertical_position)
                                         ? std::clamp(style.vertical_position, 0.0, 1.0)
                                         : std::numeric_limits<double>::quiet_NaN();
  const int y_bottom =
      std::isfinite(normalized_position) && !uses_legacy_default_position(style)
          ? static_cast<int>(std::lround(safe_top + normalized_position * (safe_bottom - safe_top)))
          : frame.height() - bottom_margin_pixels;
  const double bounded_outline_width =
      std::isfinite(style.outline_width)
          ? std::clamp(style.outline_width, 0.0,
                       static_cast<double>(std::max(frame.width(), frame.height())))
          : 0.0;
  const int outline_pixels = static_cast<int>(std::ceil(bounded_outline_width));
  const int y_start = y_bottom - total_height;

  const int padding = 4 + outline_pixels;
  fill_background_rect(frame, content_left - padding, y_start - padding,
                       content_left + total_width + padding, y_bottom + padding,
                       style.background_color);

  int line_y = y_start;
  for (const auto& line : lines) {
    const int line_width = static_cast<int>(line.size()) * cell_width;
    int line_x = aligned_left(style.alignment, safe_left, safe_right, line_width);
    for (const char32_t codepoint : line) {
      const render::Glyph glyph = render::supported_glyph(codepoint)
                                      ? render::glyph_for_ascii(codepoint)
                                      : render::replacement_glyph();
      draw_glyph_with_outline(frame, glyph, line_x, line_y, scale, style, outline_pixels);
      line_x += cell_width;
    }
    line_y += cell_height;
  }
  return std::nullopt;
}

std::optional<CaptionBurnInError> burn_in_captions(render::CpuFrame& frame,
                                                   const std::vector<edit::Caption>& captions,
                                                   const edit::Time timeline_time) {
  const int bottom_margin = std::max(1, frame.height() / 12);
  for (const auto& caption : captions) {
    if (!caption.text.empty() && caption.range.contains(timeline_time)) {
      if (const auto error = draw_caption_text(frame, caption.style, caption.text, bottom_margin)) {
        return error;
      }
    }
  }
  return std::nullopt;
}

edit::Result<CaptionSidecarResult, CaptionSidecarError>
write_caption_sidecar(const std::vector<edit::Caption>& captions,
                      const std::filesystem::path& media_path, const SidecarFormat format,
                      const edit::Time timeline_start, const edit::Time timeline_end) {
  const edit::TimeRange requested_range{timeline_start, timeline_end - timeline_start};
  std::vector<caption_service::CaptionCue> cues;
  for (const auto& caption : captions) {
    if (caption.range.overlaps(requested_range)) {
      cues.push_back(caption_service::fromEditCaption(caption));
    }
  }
  if (cues.empty()) {
    return edit::Result<CaptionSidecarResult, CaptionSidecarError>::failure(
        CaptionSidecarError::NoCaptions);
  }

  caption_service::CaptionDocument document;
  document.format = format == SidecarFormat::Srt ? caption_service::SubtitleFormat::Srt
                                                 : caption_service::SubtitleFormat::WebVtt;
  document.cues = std::move(cues);
  caption_service::SerializeOptions options;
  options.timestamp_policy = caption_service::TimestampPolicy::NearestMillisecond;

  try {
    const auto serialized_result = format == SidecarFormat::Srt
                                       ? caption_service::serializeSrt(document, options)
                                       : caption_service::serializeWebVtt(document, options);
    if (!serialized_result) {
      return edit::Result<CaptionSidecarResult, CaptionSidecarError>::failure(
          CaptionSidecarError::SerializationFailed);
    }

    std::filesystem::path sidecar_path = media_path;
    sidecar_path.replace_extension(format == SidecarFormat::Srt ? ".srt" : ".vtt");
    std::ofstream output(sidecar_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return edit::Result<CaptionSidecarResult, CaptionSidecarError>::failure(
          CaptionSidecarError::WriteFailed);
    }
    const std::string& serialized = serialized_result.value();
    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    if (!output) {
      return edit::Result<CaptionSidecarResult, CaptionSidecarError>::failure(
          CaptionSidecarError::WriteFailed);
    }
    return edit::Result<CaptionSidecarResult, CaptionSidecarError>::success(
        CaptionSidecarResult{true, sidecar_path, document.cues.size()});
  } catch (...) {
    return edit::Result<CaptionSidecarResult, CaptionSidecarError>::failure(
        CaptionSidecarError::WriteFailed);
  }
}

} // namespace video_editor::export_service
