// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include "internal.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace video_editor::caption_service {
namespace {

[[nodiscard]] std::optional<std::int64_t> millisecondsFor(edit::Time time,
                                                          const SerializeOptions& options,
                                                          std::size_t line,
                                                          std::vector<Diagnostic>& diagnostics) {
  if (time.isNegative()) {
    diagnostics.push_back({DiagnosticCode::InvalidTimestamp, DiagnosticSeverity::Error, line, 1,
                           "negative subtitle timestamps cannot be serialized"});
    return std::nullopt;
  }
  try {
    const auto milliseconds =
        time.rescaledTo(1'000, options.timestamp_policy == TimestampPolicy::NearestMillisecond
                                   ? edit::RoundingMode::NearestTiesEven
                                   : edit::RoundingMode::TowardZero);
    if (options.timestamp_policy == TimestampPolicy::RequireExactMilliseconds &&
        milliseconds != time) {
      diagnostics.push_back({DiagnosticCode::TimeNotRepresentable, DiagnosticSeverity::Error, line,
                             1, "timestamp is not exactly representable in subtitle milliseconds"});
      return std::nullopt;
    }
    return milliseconds.value();
  } catch (const std::exception&) {
    diagnostics.push_back({DiagnosticCode::TimeNotRepresentable, DiagnosticSeverity::Error, line, 1,
                           "timestamp exceeds the subtitle time range"});
    return std::nullopt;
  }
}

[[nodiscard]] std::string formatTimestamp(std::int64_t milliseconds, SubtitleFormat format) {
  const auto hours = milliseconds / 3'600'000;
  milliseconds %= 3'600'000;
  const auto minutes = milliseconds / 60'000;
  milliseconds %= 60'000;
  const auto seconds = milliseconds / 1'000;
  const auto fraction = milliseconds % 1'000;

  std::ostringstream output;
  output << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':'
         << std::setw(2) << seconds << (format == SubtitleFormat::Srt ? ',' : '.') << std::setw(3)
         << fraction;
  return output.str();
}

void validateSerializableStrings(const CaptionDocument& document, SubtitleFormat format,
                                 std::vector<Diagnostic>& diagnostics) {
  const auto valid_single_line = [](std::string_view value) {
    return detail::isValidUtf8(value) && value.find_first_of("\r\n") == std::string_view::npos;
  };
  if (format == SubtitleFormat::WebVtt &&
      (!valid_single_line(document.header) ||
       std::ranges::any_of(document.header_metadata,
                           [&](const auto& line) { return !valid_single_line(line); }))) {
    diagnostics.push_back({DiagnosticCode::InvalidUtf8, DiagnosticSeverity::Error, 1, 1,
                           "WebVTT header contains invalid UTF-8 or a newline"});
  }
  for (std::size_t index = 0; index < document.cues.size(); ++index) {
    const auto& cue = document.cues[index];
    const auto line = cue.source_line == 0 ? index + 1 : cue.source_line;
    if (!detail::isValidUtf8(cue.text) || !detail::isValidUtf8(cue.settings) ||
        (cue.identifier && !detail::isValidUtf8(*cue.identifier))) {
      diagnostics.push_back({DiagnosticCode::InvalidUtf8, DiagnosticSeverity::Error, line, 1,
                             "cue contains invalid UTF-8"});
    }
    if (cue.identifier && cue.identifier->find_first_of("\r\n") != std::string::npos) {
      diagnostics.push_back({DiagnosticCode::InvalidCueBlock, DiagnosticSeverity::Error, line, 1,
                             "cue identifier must fit on one line"});
    }
    if (cue.settings.find_first_of("\r\n") != std::string::npos) {
      diagnostics.push_back({DiagnosticCode::InvalidCueBlock, DiagnosticSeverity::Error, line, 1,
                             "cue settings must fit on one line"});
    }
  }
}

[[nodiscard]] std::string normalizeTextLineEndings(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\r') {
      normalized.push_back('\n');
      if (index + 1 < text.size() && text[index + 1] == '\n') {
        ++index;
      }
    } else {
      normalized.push_back(text[index]);
    }
  }
  return normalized;
}

} // namespace

SerializeResult serialize(const CaptionDocument& document, SubtitleFormat format,
                          const SerializeOptions& options) {
  auto diagnostics = validate(document, options.validation);
  validateSerializableStrings(document, format, diagnostics);

  struct SerializedTime final {
    std::int64_t start{0};
    std::int64_t end{0};
  };
  std::vector<SerializedTime> times;
  times.reserve(document.cues.size());
  for (std::size_t index = 0; index < document.cues.size(); ++index) {
    const auto& cue = document.cues[index];
    const auto line = cue.source_line == 0 ? index + 1 : cue.source_line;
    const auto start = millisecondsFor(cue.range.start, options, line, diagnostics);
    const auto end = millisecondsFor(cue.range.end(), options, line, diagnostics);
    if (start && end && *end <= *start) {
      diagnostics.push_back({DiagnosticCode::EndNotAfterStart, DiagnosticSeverity::Error, line, 1,
                             "rounded cue end must be after its start"});
    }
    times.push_back({start.value_or(0), end.value_or(0)});
  }
  if (detail::hasErrors(diagnostics)) {
    return SerializeResult::failure(std::move(diagnostics));
  }

  std::string output;
  if (options.emit_utf8_bom) {
    output.append("\xEF\xBB\xBF", 3);
  }
  if (format == SubtitleFormat::WebVtt) {
    output += "WEBVTT";
    if (!document.header.empty()) {
      output.push_back(' ');
      output += document.header;
    }
    output.push_back('\n');
    for (const auto& line : document.header_metadata) {
      output += line;
      output.push_back('\n');
    }
    output.push_back('\n');
  }

  for (std::size_t index = 0; index < document.cues.size(); ++index) {
    const auto& cue = document.cues[index];
    if (format == SubtitleFormat::Srt) {
      output += cue.identifier.value_or(std::to_string(index + 1));
      output.push_back('\n');
    } else if (cue.identifier) {
      output += *cue.identifier;
      output.push_back('\n');
    }
    output += formatTimestamp(times[index].start, format);
    output += " --> ";
    output += formatTimestamp(times[index].end, format);
    if (!cue.settings.empty()) {
      output.push_back(' ');
      output += cue.settings;
    }
    output.push_back('\n');
    output += normalizeTextLineEndings(cue.text);
    output += "\n\n";
  }
  return SerializeResult::success(std::move(output));
}

SerializeResult serializeSrt(const CaptionDocument& document, const SerializeOptions& options) {
  return serialize(document, SubtitleFormat::Srt, options);
}

SerializeResult serializeWebVtt(const CaptionDocument& document, const SerializeOptions& options) {
  return serialize(document, SubtitleFormat::WebVtt, options);
}

} // namespace video_editor::caption_service
