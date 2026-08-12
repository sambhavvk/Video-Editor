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

} // namespace video_editor::caption_service::detail
