// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/caption_service/caption_service.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::caption_service::detail {

struct SourceLine final {
  std::string text;
  std::size_t number{0};
};

struct NormalizedInput final {
  std::string text;
  std::vector<SourceLine> lines;
};

[[nodiscard]] bool isValidUtf8(std::string_view text) noexcept;
[[nodiscard]] std::size_t utf8Length(std::string_view text) noexcept;
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;
[[nodiscard]] std::optional<NormalizedInput> normalizeInput(std::string_view input,
                                                            std::vector<Diagnostic>& diagnostics);
[[nodiscard]] std::optional<edit::Time> parseSrtTimestamp(std::string_view text) noexcept;
[[nodiscard]] std::optional<edit::Time> parseWebVttTimestamp(std::string_view text) noexcept;

struct TimingLine final {
  edit::Time start;
  edit::Time end;
  std::string settings;
};

[[nodiscard]] std::optional<TimingLine> parseTimingLine(std::string_view text,
                                                        SubtitleFormat format, std::size_t line,
                                                        std::vector<Diagnostic>& diagnostics);

[[nodiscard]] bool hasErrors(std::span<const Diagnostic> diagnostics) noexcept;

// Derives a deterministic child id from a source entity and a fragment index,
// preserving UUID version/variant bits. Distinct fragment indices yield
// distinct ids; identical inputs always produce the same id.
[[nodiscard]] edit::EntityId fragmentId(edit::EntityId source, std::size_t fragment) noexcept;

// Converts an exact timeline duration into the matching source-media duration
// for a clip, accounting for its playback rate. Direction (reverse) is handled
// by the caller when mapping into a source range.
[[nodiscard]] edit::Time sourceDelta(const edit::Clip& clip, edit::Time timeline_delta);

// Computes the source range for the portion of a clip covered by the timeline
// sub-range [timeline_in, timeline_out), which must lie within the clip's
// timeline range. Honors reversed clips.
[[nodiscard]] edit::TimeRange sourceRangeForSubClip(const edit::Clip& clip,
                                                    edit::Time timeline_in,
                                                    edit::Time timeline_out);

} // namespace video_editor::caption_service::detail
