// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include "internal.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>

namespace video_editor::caption_service {
namespace {

[[nodiscard]] bool isWhitespace(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
         character == '\f' || character == '\v';
}

[[nodiscard]] std::vector<std::string> words(std::string_view text) {
  std::vector<std::string> result;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && isWhitespace(text[index])) {
      ++index;
    }
    const auto begin = index;
    while (index < text.size() && !isWhitespace(text[index])) {
      ++index;
    }
    if (begin < index) {
      result.emplace_back(text.substr(begin, index - begin));
    }
  }
  return result;
}

[[nodiscard]] std::vector<std::string> wrap(std::string_view text, const ReflowOptions& options) {
  std::vector<std::string> lines;
  for (auto& word : words(text)) {
    if (lines.empty()) {
      lines.push_back(std::move(word));
      continue;
    }
    const auto candidate_length = detail::utf8Length(lines.back()) + 1 + detail::utf8Length(word);
    if (candidate_length <= options.max_characters_per_line) {
      lines.back().push_back(' ');
      lines.back() += word;
    } else {
      lines.push_back(std::move(word));
    }
  }
  return lines;
}

[[nodiscard]] std::vector<std::string> pagesFor(std::string_view text,
                                                const ReflowOptions& options) {
  const auto lines = wrap(text, options);
  std::vector<std::string> pages;
  for (std::size_t begin = 0; begin < lines.size(); begin += options.max_lines) {
    std::string page;
    const auto end = std::min(lines.size(), begin + options.max_lines);
    for (auto index = begin; index < end; ++index) {
      if (!page.empty()) {
        page.push_back('\n');
      }
      page += lines[index];
    }
    pages.push_back(std::move(page));
  }
  if (pages.empty()) {
    pages.emplace_back();
  }
  return pages;
}

[[nodiscard]] std::string splitIdentifier(const std::string& identifier, std::size_t page_index) {
  if (page_index == 0) {
    return identifier;
  }
  return identifier + '-' + std::to_string(page_index + 1);
}

[[nodiscard]] ReflowError makeError(ReflowErrorCode code, std::size_t cue_index,
                                    std::string message) {
  return ReflowError{code, cue_index, std::move(message)};
}

[[nodiscard]] std::optional<edit::Time> proportionalOffset(edit::Time duration, std::size_t portion,
                                                           std::size_t total) {
  auto reduced = duration.normalized();
  auto value = static_cast<std::uint64_t>(reduced.value());
  auto timescale = static_cast<std::uint64_t>(reduced.timescale());
  auto numerator = static_cast<std::uint64_t>(portion);
  auto denominator = static_cast<std::uint64_t>(total);

  const auto first_divisor = std::gcd(value, denominator);
  value /= first_divisor;
  denominator /= first_divisor;
  const auto second_divisor = std::gcd(numerator, timescale);
  numerator /= second_divisor;
  timescale /= second_divisor;

  const auto max_value = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (numerator != 0 && value > max_value / numerator) {
    return std::nullopt;
  }
  if (denominator != 0 && timescale > std::numeric_limits<std::uint32_t>::max() / denominator) {
    return std::nullopt;
  }
  return edit::Time(static_cast<std::int64_t>(value * numerator),
                    static_cast<std::uint32_t>(timescale * denominator));
}

} // namespace

ReflowResult reflow(const CaptionDocument& document, const ReflowOptions& options) {
  if (options.max_characters_per_line == 0 || options.max_lines == 0) {
    return ReflowResult::failure(makeError(ReflowErrorCode::InvalidOptions, 0,
                                           "reflow limits must both be greater than zero"));
  }

  CaptionDocument result = document;
  result.cues.clear();
  for (std::size_t cue_index = 0; cue_index < document.cues.size(); ++cue_index) {
    const auto& source = document.cues[cue_index];
    if (!detail::isValidUtf8(source.text)) {
      return ReflowResult::failure(
          makeError(ReflowErrorCode::InvalidUtf8, cue_index, "caption text is not valid UTF-8"));
    }
    auto pages = pagesFor(source.text, options);
    if (pages.size() == 1) {
      auto cue = source;
      cue.text = std::move(pages.front());
      result.cues.push_back(std::move(cue));
      continue;
    }

    std::vector<std::size_t> weights;
    weights.reserve(pages.size());
    for (const auto& page : pages) {
      weights.push_back(std::max<std::size_t>(1, detail::utf8Length(page)));
    }
    const auto total = std::accumulate(weights.begin(), weights.end(), std::size_t{0});
    if (total > std::numeric_limits<std::uint32_t>::max() ||
        total > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      return ReflowResult::failure(
          makeError(ReflowErrorCode::ArithmeticOverflow, cue_index,
                    "cue text is too large to allocate exact split timing"));
    }

    edit::Time previous_offset{};
    std::size_t cumulative = 0;
    try {
      for (std::size_t page_index = 0; page_index < pages.size(); ++page_index) {
        cumulative += weights[page_index];
        const auto calculated_offset =
            page_index + 1 == pages.size()
                ? std::optional<edit::Time>{source.range.duration}
                : proportionalOffset(source.range.duration, cumulative, total);
        if (!calculated_offset) {
          return ReflowResult::failure(makeError(ReflowErrorCode::ArithmeticOverflow, cue_index,
                                                 "cue split timing cannot be represented exactly"));
        }
        const auto current_offset = *calculated_offset;
        if (current_offset <= previous_offset) {
          return ReflowResult::failure(
              makeError(ReflowErrorCode::InsufficientCueDuration, cue_index,
                        "cue duration is too short for the requested reflow split"));
        }

        CaptionCue cue = source;
        cue.text = std::move(pages[page_index]);
        cue.range =
            edit::TimeRange(source.range.start + previous_offset, current_offset - previous_offset);
        if (source.identifier) {
          cue.identifier = splitIdentifier(*source.identifier, page_index);
        }
        result.cues.push_back(std::move(cue));
        previous_offset = current_offset;
      }
    } catch (const std::exception&) {
      return ReflowResult::failure(makeError(ReflowErrorCode::ArithmeticOverflow, cue_index,
                                             "cue timing overflowed while reflowing"));
    }
  }
  return ReflowResult::success(std::move(result));
}

} // namespace video_editor::caption_service
