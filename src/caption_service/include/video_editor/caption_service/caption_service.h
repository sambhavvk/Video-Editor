// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/result.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::caption_service {

enum class SubtitleFormat { Srt, WebVtt };

enum class DiagnosticSeverity { Warning, Error };

enum class DiagnosticCode {
  EmptyInput,
  InvalidUtf8,
  MissingWebVttHeader,
  InvalidCueBlock,
  MissingTiming,
  InvalidTimestamp,
  EndNotAfterStart,
  EmptyCueText,
  CueOutOfOrder,
  CueOverlap,
  DuplicateIdentifier,
  TimeNotRepresentable,
};

struct Diagnostic final {
  DiagnosticCode code{DiagnosticCode::InvalidCueBlock};
  DiagnosticSeverity severity{DiagnosticSeverity::Error};
  std::size_t line{0};
  std::size_t column{0};
  std::string message;

  friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

struct CaptionCue final {
  std::optional<std::string> identifier;
  edit::TimeRange range;
  std::string text;
  // The tokens after a WebVTT timing line (for example, "line:90% align:center")
  // or non-standard SRT positioning tokens. The service preserves this field
  // verbatim apart from surrounding whitespace.
  std::string settings;
  // One-based line in the parsed source. Generated cues use zero.
  std::size_t source_line{0};

  friend bool operator==(const CaptionCue& lhs, const CaptionCue& rhs) {
    return lhs.identifier == rhs.identifier && lhs.range == rhs.range && lhs.text == rhs.text &&
           lhs.settings == rhs.settings;
  }
};

struct CaptionDocument final {
  SubtitleFormat format{SubtitleFormat::Srt};
  // Text following WEBVTT on the first line, without surrounding whitespace.
  std::string header;
  // WebVTT metadata lines between the WEBVTT header and the first blank line.
  std::vector<std::string> header_metadata;
  std::string language;
  std::vector<CaptionCue> cues;

  friend bool operator==(const CaptionDocument&, const CaptionDocument&) = default;
};

struct ValidationOptions final {
  bool require_chronological_order{true};
  bool allow_overlaps{false};
  bool require_non_empty_text{true};
  bool require_unique_identifiers{true};
};

struct ParseOptions final {
  ValidationOptions validation{};
};

using ParseResult = edit::Result<CaptionDocument, std::vector<Diagnostic>>;

// Input must be UTF-8. UTF-8 BOM, CRLF, and CR line endings are accepted.
[[nodiscard]] ParseResult parse(std::string_view input, SubtitleFormat format,
                                const ParseOptions& options = {});
[[nodiscard]] ParseResult parseSrt(std::string_view input, const ParseOptions& options = {});
[[nodiscard]] ParseResult parseWebVtt(std::string_view input, const ParseOptions& options = {});

[[nodiscard]] std::vector<Diagnostic> validate(const CaptionDocument& document,
                                               const ValidationOptions& options = {});

enum class TimestampPolicy {
  RequireExactMilliseconds,
  NearestMillisecond,
};

struct SerializeOptions final {
  TimestampPolicy timestamp_policy{TimestampPolicy::RequireExactMilliseconds};
  bool emit_utf8_bom{false};
  ValidationOptions validation{};
};

using SerializeResult = edit::Result<std::string, std::vector<Diagnostic>>;

// Output is normalized UTF-8 with LF line endings and a trailing blank line.
[[nodiscard]] SerializeResult serialize(const CaptionDocument& document, SubtitleFormat format,
                                        const SerializeOptions& options = {});
[[nodiscard]] SerializeResult serializeSrt(const CaptionDocument& document,
                                           const SerializeOptions& options = {});
[[nodiscard]] SerializeResult serializeWebVtt(const CaptionDocument& document,
                                              const SerializeOptions& options = {});

struct ReflowOptions final {
  std::size_t max_characters_per_line{42};
  std::size_t max_lines{2};
};

enum class ReflowErrorCode {
  InvalidOptions,
  InvalidUtf8,
  InsufficientCueDuration,
  ArithmeticOverflow,
};

struct ReflowError final {
  ReflowErrorCode code{ReflowErrorCode::InvalidOptions};
  std::size_t cue_index{0};
  std::string message;

  friend bool operator==(const ReflowError&, const ReflowError&) = default;
};

using ReflowResult = edit::Result<CaptionDocument, ReflowError>;

// Rewraps on word boundaries. If a cue needs more than max_lines, it is split
// into contiguous cues whose timing is allocated proportionally by text length.
// Words are never split; a single overlong word therefore exceeds the line cap.
[[nodiscard]] ReflowResult reflow(const CaptionDocument& document,
                                  const ReflowOptions& options = {});

struct SearchOptions final {
  bool case_sensitive{false};
  bool whole_word{false};
};

struct SearchHit final {
  std::size_t cue_index{0};
  edit::TimeRange range;
  std::size_t byte_offset{0};
  std::size_t byte_length{0};
  std::string matched_text;

  friend bool operator==(const SearchHit&, const SearchHit&) = default;
};

enum class SearchErrorCode { EmptyQuery, InvalidUtf8 };

struct SearchError final {
  SearchErrorCode code{SearchErrorCode::EmptyQuery};
  std::string message;

  friend bool operator==(const SearchError&, const SearchError&) = default;
};

using SearchResult = edit::Result<std::vector<SearchHit>, SearchError>;

// Case-insensitive search folds ASCII letters and matches other UTF-8 code
// points exactly. Hit offsets are UTF-8 byte offsets into CaptionCue::text.
[[nodiscard]] SearchResult search(std::span<const CaptionCue> cues, std::string_view query,
                                  const SearchOptions& options = {});
[[nodiscard]] SearchResult search(const CaptionDocument& document, std::string_view query,
                                  const SearchOptions& options = {});

[[nodiscard]] edit::Caption toEditCaption(const CaptionCue& cue, std::string language = {},
                                          const edit::CaptionStyle& style = {});
[[nodiscard]] CaptionCue fromEditCaption(const edit::Caption& caption);
[[nodiscard]] std::vector<edit::Caption> toEditCaptions(const CaptionDocument& document,
                                                        const edit::CaptionStyle& style = {});
[[nodiscard]] CaptionDocument fromEditCaptions(std::span<const edit::Caption> captions,
                                               SubtitleFormat format, std::string language = {});

} // namespace video_editor::caption_service
