// SPDX-License-Identifier: MPL-2.0
#include "internal.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

namespace video_editor::caption_service::detail {
namespace {

[[nodiscard]] bool isContinuation(unsigned char byte) noexcept {
  return (byte & 0xC0U) == 0x80U;
}

[[nodiscard]] bool parseUnsigned(std::string_view text, std::uint64_t& value) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] std::optional<edit::Time> makeTimestamp(std::uint64_t hours, std::uint64_t minutes,
                                                      std::uint64_t seconds,
                                                      std::string_view fraction) noexcept {
  if (minutes > 59 || seconds > 59 || fraction.empty() || fraction.size() > 3) {
    return std::nullopt;
  }
  std::uint64_t fraction_value = 0;
  if (!parseUnsigned(fraction, fraction_value)) {
    return std::nullopt;
  }
  for (auto digits = fraction.size(); digits < 3; ++digits) {
    fraction_value *= 10;
  }

  constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (hours > max / 3'600'000ULL) {
    return std::nullopt;
  }
  auto milliseconds = hours * 3'600'000ULL;
  const auto remainder = minutes * 60'000ULL + seconds * 1'000ULL + fraction_value;
  if (milliseconds > max - remainder) {
    return std::nullopt;
  }
  milliseconds += remainder;
  return edit::Time(static_cast<std::int64_t>(milliseconds), 1'000);
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (true) {
    const auto end = text.find(delimiter, begin);
    if (end == std::string_view::npos) {
      parts.push_back(text.substr(begin));
      return parts;
    }
    parts.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
}

[[nodiscard]] std::optional<edit::Time> parseTimestamp(std::string_view text,
                                                       bool web_vtt) noexcept {
  text = trim(text);
  const auto separator = text.find_last_of(web_vtt ? "." : ",.");
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  const auto clock = text.substr(0, separator);
  const auto fraction = text.substr(separator + 1);
  const auto parts = split(clock, ':');
  if ((!web_vtt && parts.size() != 3) || (web_vtt && parts.size() != 2 && parts.size() != 3)) {
    return std::nullopt;
  }

  std::uint64_t hours = 0;
  std::uint64_t minutes = 0;
  std::uint64_t seconds = 0;
  if (parts.size() == 3) {
    if (parts[0].size() < 2 || parts[1].size() != 2 || parts[2].size() != 2 ||
        !parseUnsigned(parts[0], hours) || !parseUnsigned(parts[1], minutes) ||
        !parseUnsigned(parts[2], seconds)) {
      return std::nullopt;
    }
  } else {
    if (parts[0].size() != 2 || parts[1].size() != 2 || !parseUnsigned(parts[0], minutes) ||
        !parseUnsigned(parts[1], seconds)) {
      return std::nullopt;
    }
  }
  return makeTimestamp(hours, minutes, seconds, fraction);
}

} // namespace

bool isValidUtf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
      if (index + 1 >= text.size() ||
          !isContinuation(static_cast<unsigned char>(text[index + 1]))) {
        return false;
      }
      index += 2;
      continue;
    }
    if (first >= 0xE0U && first <= 0xEFU) {
      if (index + 2 >= text.size()) {
        return false;
      }
      const auto second = static_cast<unsigned char>(text[index + 1]);
      const auto third = static_cast<unsigned char>(text[index + 2]);
      if (!isContinuation(second) || !isContinuation(third) || (first == 0xE0U && second < 0xA0U) ||
          (first == 0xEDU && second >= 0xA0U)) {
        return false;
      }
      index += 3;
      continue;
    }
    if (first >= 0xF0U && first <= 0xF4U) {
      if (index + 3 >= text.size()) {
        return false;
      }
      const auto second = static_cast<unsigned char>(text[index + 1]);
      const auto third = static_cast<unsigned char>(text[index + 2]);
      const auto fourth = static_cast<unsigned char>(text[index + 3]);
      if (!isContinuation(second) || !isContinuation(third) || !isContinuation(fourth) ||
          (first == 0xF0U && second < 0x90U) || (first == 0xF4U && second > 0x8FU)) {
        return false;
      }
      index += 4;
      continue;
    }
    return false;
  }
  return true;
}

std::size_t utf8Length(std::string_view text) noexcept {
  std::size_t count = 0;
  for (const auto character : text) {
    if ((static_cast<unsigned char>(character) & 0xC0U) != 0x80U) {
      ++count;
    }
  }
  return count;
}

std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

std::optional<NormalizedInput> normalizeInput(std::string_view input,
                                              std::vector<Diagnostic>& diagnostics) {
  constexpr std::string_view bom{"\xEF\xBB\xBF", 3};
  if (input.starts_with(bom)) {
    input.remove_prefix(bom.size());
  }
  if (!isValidUtf8(input)) {
    diagnostics.push_back({DiagnosticCode::InvalidUtf8, DiagnosticSeverity::Error, 1, 1,
                           "subtitle input is not valid UTF-8"});
    return std::nullopt;
  }

  NormalizedInput result;
  result.text.reserve(input.size());
  for (std::size_t index = 0; index < input.size(); ++index) {
    if (input[index] == '\r') {
      result.text.push_back('\n');
      if (index + 1 < input.size() && input[index + 1] == '\n') {
        ++index;
      }
    } else {
      result.text.push_back(input[index]);
    }
  }
  if (trim(result.text).empty()) {
    diagnostics.push_back(
        {DiagnosticCode::EmptyInput, DiagnosticSeverity::Error, 1, 1, "subtitle input is empty"});
    return std::nullopt;
  }

  std::size_t begin = 0;
  std::size_t number = 1;
  while (begin <= result.text.size()) {
    const auto end = result.text.find('\n', begin);
    if (end == std::string::npos) {
      result.lines.push_back({result.text.substr(begin), number});
      break;
    }
    result.lines.push_back({result.text.substr(begin, end - begin), number});
    begin = end + 1;
    ++number;
  }
  return result;
}

std::optional<edit::Time> parseSrtTimestamp(std::string_view text) noexcept {
  return parseTimestamp(text, false);
}

std::optional<edit::Time> parseWebVttTimestamp(std::string_view text) noexcept {
  return parseTimestamp(text, true);
}

std::optional<TimingLine> parseTimingLine(std::string_view text, SubtitleFormat format,
                                          std::size_t line, std::vector<Diagnostic>& diagnostics) {
  const auto arrow = text.find("-->");
  if (arrow == std::string_view::npos || text.find("-->", arrow + 3) != std::string_view::npos) {
    diagnostics.push_back({DiagnosticCode::MissingTiming, DiagnosticSeverity::Error, line, 1,
                           "cue timing line must contain exactly one -->"});
    return std::nullopt;
  }
  const auto start_text = trim(text.substr(0, arrow));
  auto end_and_settings = trim(text.substr(arrow + 3));
  const auto setting_start = end_and_settings.find_first_of(" \t");
  const auto end_text = end_and_settings.substr(0, setting_start);
  const auto settings = setting_start == std::string_view::npos
                            ? std::string_view{}
                            : trim(end_and_settings.substr(setting_start + 1));

  const auto parse_timestamp =
      format == SubtitleFormat::Srt ? parseSrtTimestamp : parseWebVttTimestamp;
  const auto start = parse_timestamp(start_text);
  const auto end = parse_timestamp(end_text);
  if (!start || !end) {
    diagnostics.push_back({DiagnosticCode::InvalidTimestamp, DiagnosticSeverity::Error, line, 1,
                           "cue has an invalid timestamp"});
    return std::nullopt;
  }
  if (*end <= *start) {
    diagnostics.push_back({DiagnosticCode::EndNotAfterStart, DiagnosticSeverity::Error, line, 1,
                           "cue end time must be after its start time"});
    return std::nullopt;
  }
  return TimingLine{*start, *end, std::string(settings)};
}

bool hasErrors(std::span<const Diagnostic> diagnostics) noexcept {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == DiagnosticSeverity::Error) {
      return true;
    }
  }
  return false;
}

} // namespace video_editor::caption_service::detail
