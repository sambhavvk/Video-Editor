// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include <gtest/gtest.h>

#include <string>

namespace captions = video_editor::caption_service;
namespace edit = video_editor::edit;

namespace {

[[nodiscard]] captions::CaptionCue cue(std::string text, edit::Time start = edit::Time{},
                                       edit::Time duration = edit::Time(4, 1)) {
  return {
      .identifier = "cue-id", .range = edit::TimeRange(start, duration), .text = std::move(text)};
}

TEST(CaptionReflow, WrapsAndSplitsWithoutBreakingWords) {
  captions::CaptionDocument document;
  document.cues.push_back(cue("aa bb cc dd"));

  const auto result = captions::reflow(document, {.max_characters_per_line = 5, .max_lines = 1});
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().cues.size(), 2U);
  EXPECT_EQ(result.value().cues[0].text, "aa bb");
  EXPECT_EQ(result.value().cues[1].text, "cc dd");
  EXPECT_EQ(result.value().cues[0].range, edit::TimeRange(edit::Time{}, edit::Time(2, 1)));
  EXPECT_EQ(result.value().cues[1].range, edit::TimeRange(edit::Time(2, 1), edit::Time(2, 1)));
  EXPECT_EQ(result.value().cues[0].identifier, "cue-id");
  EXPECT_EQ(result.value().cues[1].identifier, "cue-id-2");
  EXPECT_EQ(result.value().cues[0].range.end(), result.value().cues[1].range.start);
  EXPECT_EQ(result.value().cues[1].range.end(), document.cues[0].range.end());
}

TEST(CaptionReflow, CountsUnicodeCodePointsAndKeepsOverlongWordsWhole) {
  captions::CaptionDocument document;
  document.cues.push_back(cue("éé éé"));
  document.cues.push_back(cue("extraordinarily", edit::Time(5, 1)));

  const auto result = captions::reflow(document, {.max_characters_per_line = 5, .max_lines = 1});
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().cues.size(), 2U);
  EXPECT_EQ(result.value().cues[0].text, "éé éé");
  EXPECT_EQ(result.value().cues[1].text, "extraordinarily");
}

TEST(CaptionReflow, RejectsInvalidLimitsAndKeepsShortSplitsExact) {
  captions::CaptionDocument document;
  document.cues.push_back(cue("aa bb", edit::Time{}, edit::Time(1, 1'000)));

  const auto invalid = captions::reflow(document, {.max_characters_per_line = 0, .max_lines = 1});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code, captions::ReflowErrorCode::InvalidOptions);

  const auto too_short = captions::reflow(document, {.max_characters_per_line = 2, .max_lines = 1});
  ASSERT_TRUE(too_short);
  ASSERT_EQ(too_short.value().cues.size(), 2U);
  EXPECT_EQ(too_short.value().cues[0].range.duration, edit::Time(1, 2'000));
  EXPECT_EQ(too_short.value().cues[1].range.start, edit::Time(1, 2'000));
  EXPECT_EQ(too_short.value().cues[1].range.end(), edit::Time(1, 1'000));
}

TEST(CaptionSearch, ReturnsCaseInsensitiveCueAndTimeHits) {
  captions::CaptionDocument document;
  document.cues.push_back(cue("Hello world, HELLO creators!"));
  document.cues.push_back(cue("Nothing here", edit::Time(10, 1), edit::Time(2, 1)));

  const auto result = captions::search(document, "hello");
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 2U);
  EXPECT_EQ(result.value()[0].cue_index, 0U);
  EXPECT_EQ(result.value()[0].byte_offset, 0U);
  EXPECT_EQ(result.value()[0].matched_text, "Hello");
  EXPECT_EQ(result.value()[1].matched_text, "HELLO");
  EXPECT_EQ(result.value()[0].range, document.cues[0].range);
}

TEST(CaptionSearch, SupportsWholeWordsAndUtf8ByteOffsets) {
  captions::CaptionDocument document;
  document.cues.push_back(cue("café cat concatenate cat"));

  const auto result =
      captions::search(document, "cat", {.case_sensitive = true, .whole_word = true});
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 2U);
  EXPECT_EQ(result.value()[0].matched_text, "cat");
  EXPECT_EQ(result.value()[0].byte_offset, 6U);
  EXPECT_EQ(result.value()[1].byte_offset, 22U);
}

TEST(CaptionSearch, RejectsEmptyAndMalformedQueries) {
  const std::vector<captions::CaptionCue> cues{cue("text")};
  const auto empty = captions::search(cues, "");
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().code, captions::SearchErrorCode::EmptyQuery);

  const std::string malformed{"\xC0\xAF", 2};
  const auto invalid = captions::search(cues, malformed);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code, captions::SearchErrorCode::InvalidUtf8);
}

TEST(CaptionConversion, ConvertsToAndFromEditCaptions) {
  const auto identifier = edit::EntityId::generate();
  captions::CaptionCue source{.identifier = identifier.toString(),
                              .range = edit::TimeRange(edit::Time(2, 1), edit::Time(3, 1)),
                              .text = "Caption text"};
  edit::CaptionStyle style;
  style.bold = true;

  const auto converted = captions::toEditCaption(source, "en-GB", style);
  EXPECT_EQ(converted.id, identifier);
  EXPECT_EQ(converted.range, source.range);
  EXPECT_EQ(converted.text, source.text);
  EXPECT_EQ(converted.language, "en-GB");
  EXPECT_TRUE(converted.style.bold);
  EXPECT_EQ(captions::fromEditCaption(converted), source);

  captions::CaptionDocument document;
  document.format = captions::SubtitleFormat::WebVtt;
  document.language = "en-GB";
  document.cues.push_back(source);
  const auto edit_captions = captions::toEditCaptions(document, style);
  const auto restored = captions::fromEditCaptions(edit_captions, captions::SubtitleFormat::WebVtt);
  EXPECT_EQ(restored.language, "en-GB");
  ASSERT_EQ(restored.cues.size(), 1U);
  EXPECT_EQ(restored.cues[0], source);
}

} // namespace
