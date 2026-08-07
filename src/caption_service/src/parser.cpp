// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include "internal.h"

#include <algorithm>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>

namespace video_editor::caption_service {
namespace {

[[nodiscard]] bool isBlank(std::string_view text) noexcept {
  return detail::trim(text).empty();
}

[[nodiscard]] bool hasVisibleText(std::string_view text) noexcept {
  return std::ranges::any_of(text, [](char character) {
    return character != ' ' && character != '\t' && character != '\n' && character != '\r';
  });
}

void skipBlankLines(std::span<const detail::SourceLine> lines, std::size_t& index) noexcept {
  while (index < lines.size() && isBlank(lines[index].text)) {
    ++index;
  }
}

void recoverToNextBlock(std::span<const detail::SourceLine> lines, std::size_t& index) noexcept {
  while (index < lines.size() && !isBlank(lines[index].text)) {
    ++index;
  }
  skipBlankLines(lines, index);
}

[[nodiscard]] std::string readCueText(std::span<const detail::SourceLine> lines,
                                      std::size_t& index) {
  std::string text;
  while (index < lines.size() && !isBlank(lines[index].text)) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    text += lines[index].text;
    ++index;
  }
  return text;
}

void parseCueBlocks(std::span<const detail::SourceLine> lines, std::size_t index,
                    SubtitleFormat format, CaptionDocument& document,
                    std::vector<Diagnostic>& diagnostics) {
  skipBlankLines(lines, index);
  while (index < lines.size()) {
    CaptionCue cue;
    cue.source_line = lines[index].number;

    if (lines[index].text.find("-->") == std::string::npos) {
      const auto identifier = detail::trim(lines[index].text);
      if (!identifier.empty()) {
        cue.identifier = std::string(identifier);
      }
      ++index;
      if (index >= lines.size() || isBlank(lines[index].text)) {
        diagnostics.push_back({DiagnosticCode::MissingTiming, DiagnosticSeverity::Error,
                               cue.source_line, 1, "cue identifier is not followed by timing"});
        recoverToNextBlock(lines, index);
        continue;
      }
    }

    const auto timing_line = lines[index].number;
    const auto timing =
        detail::parseTimingLine(lines[index].text, format, timing_line, diagnostics);
    ++index;
    if (!timing) {
      recoverToNextBlock(lines, index);
      continue;
    }
    cue.range = edit::TimeRange(timing->start, timing->end - timing->start);
    cue.settings = timing->settings;
    cue.text = readCueText(lines, index);
    document.cues.push_back(std::move(cue));
    skipBlankLines(lines, index);
  }
}

[[nodiscard]] ParseResult parseNormalized(const detail::NormalizedInput& input,
                                          SubtitleFormat format,
                                          std::vector<Diagnostic> diagnostics,
                                          const ParseOptions& options) {
  CaptionDocument document;
  document.format = format;

  std::size_t index = 0;
  if (format == SubtitleFormat::WebVtt) {
    const auto first = std::string_view(input.lines.front().text);
    const auto valid_header =
        first == "WEBVTT" ||
        (first.starts_with("WEBVTT") && first.size() > 6 && (first[6] == ' ' || first[6] == '\t'));
    if (!valid_header) {
      diagnostics.push_back({DiagnosticCode::MissingWebVttHeader, DiagnosticSeverity::Error, 1, 1,
                             "WebVTT input must begin with WEBVTT"});
      return ParseResult::failure(std::move(diagnostics));
    }
    document.header = std::string(detail::trim(first.substr(6)));
    ++index;
    while (index < input.lines.size() && !isBlank(input.lines[index].text)) {
      if (input.lines[index].text.find("-->") != std::string::npos) {
        diagnostics.push_back({DiagnosticCode::InvalidCueBlock, DiagnosticSeverity::Error,
                               input.lines[index].number, 1,
                               "a blank line must separate the WebVTT header from its first cue"});
        return ParseResult::failure(std::move(diagnostics));
      }
      document.header_metadata.push_back(input.lines[index].text);
      ++index;
    }
    skipBlankLines(input.lines, index);
  }

  parseCueBlocks(input.lines, index, format, document, diagnostics);
  auto validation = validate(document, options.validation);
  diagnostics.insert(diagnostics.end(), std::make_move_iterator(validation.begin()),
                     std::make_move_iterator(validation.end()));
  if (detail::hasErrors(diagnostics)) {
    return ParseResult::failure(std::move(diagnostics));
  }
  return ParseResult::success(std::move(document));
}

} // namespace

ParseResult parse(std::string_view input, SubtitleFormat format, const ParseOptions& options) {
  std::vector<Diagnostic> diagnostics;
  auto normalized = detail::normalizeInput(input, diagnostics);
  if (!normalized) {
    return ParseResult::failure(std::move(diagnostics));
  }
  return parseNormalized(*normalized, format, std::move(diagnostics), options);
}

ParseResult parseSrt(std::string_view input, const ParseOptions& options) {
  return parse(input, SubtitleFormat::Srt, options);
}

ParseResult parseWebVtt(std::string_view input, const ParseOptions& options) {
  return parse(input, SubtitleFormat::WebVtt, options);
}

std::vector<Diagnostic> validate(const CaptionDocument& document,
                                 const ValidationOptions& options) {
  std::vector<Diagnostic> diagnostics;
  std::unordered_map<std::string, std::size_t> identifiers;

  for (std::size_t index = 0; index < document.cues.size(); ++index) {
    const auto& cue = document.cues[index];
    const auto line = cue.source_line == 0 ? index + 1 : cue.source_line;
    if (cue.range.start.isNegative() || cue.range.duration.isZero()) {
      diagnostics.push_back({DiagnosticCode::EndNotAfterStart, DiagnosticSeverity::Error, line, 1,
                             "cue must have a non-negative start and positive "
                             "duration"});
    }
    if (options.require_non_empty_text && !hasVisibleText(cue.text)) {
      diagnostics.push_back(
          {DiagnosticCode::EmptyCueText, DiagnosticSeverity::Error, line, 1, "cue text is empty"});
    }
    if (options.require_unique_identifiers && cue.identifier) {
      const auto [iterator, inserted] = identifiers.emplace(*cue.identifier, index);
      if (!inserted) {
        diagnostics.push_back({DiagnosticCode::DuplicateIdentifier, DiagnosticSeverity::Error, line,
                               1, "cue identifier is duplicated"});
      }
      (void)iterator;
    }
    if (options.require_chronological_order && index > 0 &&
        cue.range.start < document.cues[index - 1].range.start) {
      diagnostics.push_back({DiagnosticCode::CueOutOfOrder, DiagnosticSeverity::Error, line, 1,
                             "cue starts before the preceding cue"});
    }
  }

  if (!options.allow_overlaps && document.cues.size() > 1) {
    std::vector<std::size_t> order(document.cues.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
      return document.cues[lhs].range.start < document.cues[rhs].range.start;
    });
    auto furthest = order.front();
    for (std::size_t position = 1; position < order.size(); ++position) {
      const auto current = order[position];
      if (document.cues[current].range.start < document.cues[furthest].range.end()) {
        const auto line = document.cues[current].source_line == 0
                              ? current + 1
                              : document.cues[current].source_line;
        diagnostics.push_back({DiagnosticCode::CueOverlap, DiagnosticSeverity::Error, line, 1,
                               "cue overlaps an earlier cue"});
      }
      if (document.cues[current].range.end() > document.cues[furthest].range.end()) {
        furthest = current;
      }
    }
  }
  return diagnostics;
}

} // namespace video_editor::caption_service
