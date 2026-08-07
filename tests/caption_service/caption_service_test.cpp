// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace captions = video_editor::caption_service;
namespace edit = video_editor::edit;

namespace {

[[nodiscard]] bool hasDiagnostic(const std::vector<captions::Diagnostic>& diagnostics,
                                 captions::DiagnosticCode code) {
  return std::ranges::any_of(diagnostics,
                             [&](const auto& diagnostic) { return diagnostic.code == code; });
}

TEST(CaptionParser, AcceptsSrtCommaAndDotMillisecondsExactly) {
  constexpr auto input = R"(1
00:00:01,125 --> 00:00:02.250
First

2
00:01:03.004 --> 00:01:04,050
Second
)";

  const auto parsed = captions::parseSrt(input);
  ASSERT_TRUE(parsed) << parsed.error().front().message;
  ASSERT_EQ(parsed.value().cues.size(), 2U);
  EXPECT_EQ(parsed.value().cues[0].range.start, edit::Time(1'125, 1'000));
  EXPECT_EQ(parsed.value().cues[0].range.duration, edit::Time(1'125, 1'000));
  EXPECT_EQ(parsed.value().cues[1].range.start, edit::Time(63'004, 1'000));
  EXPECT_EQ(parsed.value().cues[1].range.end(), edit::Time(64'050, 1'000));
}

TEST(CaptionParser, NormalizesBomCrLfAndPreservesUnicodeMultilineText) {
  const std::string input = "\xEF\xBB\xBF"
                            "intro\r\n"
                            "00:00:00,000 --> 00:00:01,500\r\n"
                            "Héllo 世界 👋\r\n"
                            "second line\r\n\r\n";

  const auto parsed = captions::parseSrt(input);
  ASSERT_TRUE(parsed) << parsed.error().front().message;
  ASSERT_EQ(parsed.value().cues.size(), 1U);
  EXPECT_EQ(parsed.value().cues[0].identifier, "intro");
  EXPECT_EQ(parsed.value().cues[0].text, "Héllo 世界 👋\nsecond line");

  const auto serialized = captions::serializeSrt(parsed.value());
  ASSERT_TRUE(serialized);
  EXPECT_EQ(serialized.value().find('\r'), std::string::npos);
  EXPECT_EQ(serialized.value().substr(0, 5), "intro");
}

TEST(CaptionParser, PreservesWebVttHeaderMetadataIdentifierAndSettings) {
  constexpr auto input = R"(WEBVTT Creator export
Kind: captions
Language: en

speaker-1
00:01.250 --> 00:03.500 line:90% align:center position:50%
Welcome <b>everyone</b>.

)";

  const auto parsed = captions::parseWebVtt(input);
  ASSERT_TRUE(parsed) << parsed.error().front().message;
  EXPECT_EQ(parsed.value().header, "Creator export");
  EXPECT_EQ(parsed.value().header_metadata,
            (std::vector<std::string>{"Kind: captions", "Language: en"}));
  ASSERT_EQ(parsed.value().cues.size(), 1U);
  EXPECT_EQ(parsed.value().cues[0].identifier, "speaker-1");
  EXPECT_EQ(parsed.value().cues[0].settings, "line:90% align:center position:50%");
  EXPECT_EQ(parsed.value().cues[0].range.start, edit::Time(1'250, 1'000));
}

TEST(CaptionParser, ReportsTypedDiagnosticsWithSourceLines) {
  constexpr auto input = R"(1
00:00:bad --> 00:00:02,000
Broken

)";
  const auto parsed = captions::parseSrt(input);
  ASSERT_FALSE(parsed);
  ASSERT_TRUE(hasDiagnostic(parsed.error(), captions::DiagnosticCode::InvalidTimestamp));
  const auto invalid = std::ranges::find_if(parsed.error(), [](const auto& item) {
    return item.code == captions::DiagnosticCode::InvalidTimestamp;
  });
  ASSERT_NE(invalid, parsed.error().end());
  EXPECT_EQ(invalid->line, 2U);
  EXPECT_FALSE(invalid->message.empty());
}

TEST(CaptionParser, RejectsMalformedUtf8AndMissingWebVttHeader) {
  std::string invalid_utf8 = "1\n00:00:00,000 --> 00:00:01,000\n";
  invalid_utf8.append("\xC0\xAF", 2);
  invalid_utf8.push_back('\n');
  const auto invalid = captions::parseSrt(invalid_utf8);
  ASSERT_FALSE(invalid);
  EXPECT_TRUE(hasDiagnostic(invalid.error(), captions::DiagnosticCode::InvalidUtf8));

  const auto missing = captions::parseWebVtt("00:00.000 --> 00:01.000\nNo header\n");
  ASSERT_FALSE(missing);
  EXPECT_TRUE(hasDiagnostic(missing.error(), captions::DiagnosticCode::MissingWebVttHeader));

  const auto missing_separator =
      captions::parseWebVtt("WEBVTT\n00:00.000 --> 00:01.000\nNo separator\n");
  ASSERT_FALSE(missing_separator);
  EXPECT_TRUE(hasDiagnostic(missing_separator.error(), captions::DiagnosticCode::InvalidCueBlock));
}

TEST(CaptionValidation, DetectsOrderingOverlapAndDuplicateIdentifiers) {
  constexpr auto input = R"(same
00:00:02,000 --> 00:00:04,000
Later

same
00:00:01,000 --> 00:00:03,000
Earlier and overlapping
)";
  const auto parsed = captions::parseSrt(input);
  ASSERT_FALSE(parsed);
  EXPECT_TRUE(hasDiagnostic(parsed.error(), captions::DiagnosticCode::CueOutOfOrder));
  EXPECT_TRUE(hasDiagnostic(parsed.error(), captions::DiagnosticCode::CueOverlap));
  EXPECT_TRUE(hasDiagnostic(parsed.error(), captions::DiagnosticCode::DuplicateIdentifier));

  captions::ParseOptions permissive;
  permissive.validation.require_chronological_order = false;
  permissive.validation.allow_overlaps = true;
  permissive.validation.require_unique_identifiers = false;
  EXPECT_TRUE(captions::parseSrt(input, permissive));
}

TEST(CaptionSerialization, SrtRoundTripIsDeterministic) {
  constexpr auto input = R"(alpha
00:00:00,125 --> 00:00:01,500 X1:10 X2:20
Line one
Line two

2
00:00:02,000 --> 00:00:04,025
Final

)";
  const auto first = captions::parseSrt(input);
  ASSERT_TRUE(first);
  const auto encoded_once = captions::serializeSrt(first.value());
  ASSERT_TRUE(encoded_once);
  const auto reparsed = captions::parseSrt(encoded_once.value());
  ASSERT_TRUE(reparsed);
  EXPECT_EQ(reparsed.value(), first.value());
  const auto encoded_twice = captions::serializeSrt(reparsed.value());
  ASSERT_TRUE(encoded_twice);
  EXPECT_EQ(encoded_twice.value(), encoded_once.value());
}

TEST(CaptionSerialization, WebVttRoundTripIsDeterministic) {
  constexpr auto input = R"(WEBVTT demo
Kind: captions

cue-a
00:00:00.000 --> 00:00:01.234 align:start
Héllo

)";
  const auto first = captions::parseWebVtt(input);
  ASSERT_TRUE(first);
  const auto encoded_once = captions::serializeWebVtt(first.value());
  ASSERT_TRUE(encoded_once);
  const auto reparsed = captions::parseWebVtt(encoded_once.value());
  ASSERT_TRUE(reparsed);
  EXPECT_EQ(reparsed.value(), first.value());
  const auto encoded_twice = captions::serializeWebVtt(reparsed.value());
  ASSERT_TRUE(encoded_twice);
  EXPECT_EQ(encoded_twice.value(), encoded_once.value());
}

TEST(CaptionSerialization, RequiresExplicitRoundingForFrameBasedTimes) {
  captions::CaptionDocument document;
  document.cues.push_back({.identifier = "frame",
                           .range = edit::TimeRange(edit::Time(1, 24), edit::Time(1, 1)),
                           .text = "Frame-aligned"});

  const auto exact = captions::serializeSrt(document);
  ASSERT_FALSE(exact);
  EXPECT_TRUE(hasDiagnostic(exact.error(), captions::DiagnosticCode::TimeNotRepresentable));

  captions::SerializeOptions rounded;
  rounded.timestamp_policy = captions::TimestampPolicy::NearestMillisecond;
  const auto output = captions::serializeSrt(document, rounded);
  ASSERT_TRUE(output);
  EXPECT_NE(output.value().find("00:00:00,042 --> 00:00:01,042"), std::string::npos);
}

} // namespace
