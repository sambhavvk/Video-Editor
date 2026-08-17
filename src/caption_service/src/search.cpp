// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include "internal.h"

#include <cctype>
#include <string>
#include <utility>

namespace video_editor::caption_service {
namespace {

[[nodiscard]] char foldAscii(char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return static_cast<char>(character + ('a' - 'A'));
  }
  return character;
}

[[nodiscard]] std::string folded(std::string_view text) {
  std::string result(text);
  for (auto& character : result) {
    character = foldAscii(character);
  }
  return result;
}

[[nodiscard]] bool isWordAt(std::string_view text, std::size_t byte_offset) noexcept {
  if (byte_offset >= text.size()) {
    return false;
  }
  auto first = byte_offset;
  while (first > 0 && (static_cast<unsigned char>(text[first]) & 0xC0U) == 0x80U) {
    --first;
  }
  const auto byte = static_cast<unsigned char>(text[first]);
  if (byte >= 0x80U) {
    return true;
  }
  return std::isalnum(byte) != 0 || byte == static_cast<unsigned char>('_');
}

[[nodiscard]] bool wholeWordMatch(std::string_view text, std::size_t offset,
                                  std::size_t length) noexcept {
  const auto begins_at_boundary = offset == 0 || !isWordAt(text, offset - 1);
  const auto end = offset + length;
  const auto ends_at_boundary = end == text.size() || !isWordAt(text, end);
  return begins_at_boundary && ends_at_boundary;
}

[[nodiscard]] std::optional<std::pair<edit::EntityId, edit::TimeRange>>
wordAt(const CaptionCue& cue, const std::size_t offset, const std::size_t length) {
  for (const auto& word : cue.words) {
    const auto position = cue.text.find(word.text);
    if (position != std::string::npos && offset < position + word.text.size() &&
        offset + length > position) {
      return std::pair{word.id, word.range};
    }
  }
  return std::nullopt;
}

} // namespace

SearchResult search(std::span<const CaptionCue> cues, std::string_view query,
                    const SearchOptions& options) {
  if (query.empty()) {
    return SearchResult::failure({SearchErrorCode::EmptyQuery, "search query cannot be empty"});
  }
  if (!detail::isValidUtf8(query)) {
    return SearchResult::failure({SearchErrorCode::InvalidUtf8, "search query is not valid UTF-8"});
  }

  const auto needle = options.case_sensitive ? std::string(query) : folded(query);
  std::vector<SearchHit> hits;
  for (std::size_t cue_index = 0; cue_index < cues.size(); ++cue_index) {
    const auto& cue = cues[cue_index];
    if (!detail::isValidUtf8(cue.text)) {
      return SearchResult::failure(
          {SearchErrorCode::InvalidUtf8, "caption text is not valid UTF-8"});
    }
    const auto haystack = options.case_sensitive ? cue.text : folded(cue.text);
    std::size_t offset = 0;
    while ((offset = haystack.find(needle, offset)) != std::string::npos) {
      if (!options.whole_word || wholeWordMatch(cue.text, offset, needle.size())) {
        const auto word = wordAt(cue, offset, needle.size());
        hits.push_back({cue_index, cue.range, offset, needle.size(),
                        cue.text.substr(offset, needle.size()),
                        word ? std::optional<edit::EntityId>{word->first} : std::nullopt,
                        word ? std::optional<edit::TimeRange>{word->second} : std::nullopt});
      }
      offset += needle.size();
    }
  }
  return SearchResult::success(std::move(hits));
}

SearchResult search(const CaptionDocument& document, std::string_view query,
                    const SearchOptions& options) {
  return search(document.cues, query, options);
}

} // namespace video_editor::caption_service
