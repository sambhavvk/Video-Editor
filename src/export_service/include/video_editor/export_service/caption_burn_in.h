// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/result.h"
#include "video_editor/export_service/presets.h"
#include "video_editor/render_engine/frame.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace video_editor::export_service {

enum class CaptionBurnInError : std::uint8_t {
  EmptyText,
  InvalidFontSize,
  FrameTooSmall,
};

// Draws all captions whose range contains `timeline_time` onto `frame`.
// Uses the deterministic bitmap glyph rasterizer from render_engine so the
// same caption payload produces identical pixels on every machine.
// Captions are drawn bottom-centered with a semi-transparent background box,
// matching common subtitle presentation. Multi-line text uses '\n'.
// Returns an error if the frame is too small for the requested font size;
// otherwise draws silently (unsupported glyphs render as replacement glyphs).
[[nodiscard]] std::optional<CaptionBurnInError>
burn_in_captions(render::CpuFrame& frame, const std::vector<edit::Caption>& captions,
                 edit::Time timeline_time);

// Lower-level: draws a single caption's text onto the frame at the given
// vertical anchor. Exposed for testing and custom layout.
[[nodiscard]] std::optional<CaptionBurnInError> draw_caption_text(render::CpuFrame& frame,
                                                                  const edit::CaptionStyle& style,
                                                                  std::string_view text,
                                                                  int bottom_margin_pixels);

// Sidecar caption export

enum class CaptionSidecarError : std::uint8_t {
  NoCaptions,
  SerializationFailed,
  WriteFailed,
};

struct CaptionSidecarResult {
  bool written{false};
  std::filesystem::path path;
  std::size_t cue_count{0};
};

// Writes captions overlapping [timeline_start, timeline_end) to a sidecar
// file next to `media_path` (same stem, .srt or .vtt extension).
[[nodiscard]] edit::Result<CaptionSidecarResult, CaptionSidecarError>
write_caption_sidecar(const std::vector<edit::Caption>& captions,
                      const std::filesystem::path& media_path, SidecarFormat format,
                      edit::Time timeline_start, edit::Time timeline_end);

} // namespace video_editor::export_service