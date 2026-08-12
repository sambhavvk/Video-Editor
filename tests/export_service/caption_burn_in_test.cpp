// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/caption_burn_in.h"

#include "video_editor/edit_model/model.h"
#include "video_editor/render_engine/frame.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace video_editor::export_service {
namespace {

using namespace video_editor::edit;
using namespace video_editor::render;

// Helper to create a caption with a specific time range
Caption make_caption(std::int64_t start_value, std::uint32_t start_timescale,
                     std::int64_t duration_value, std::uint32_t duration_timescale,
                     std::string text, CaptionStyle style = {}) {
  Caption caption;
  caption.range =
      TimeRange(Time(start_value, start_timescale), Time(duration_value, duration_timescale));
  caption.text = std::move(text);
  caption.style = style;
  return caption;
}

// Helper to create a caption style
CaptionStyle make_style(double font_size, ColorRgba text_color = {1.0, 1.0, 1.0, 1.0},
                        ColorRgba bg_color = {0.0, 0.0, 0.0, 0.7}) {
  CaptionStyle style;
  style.font_size = font_size;
  style.text_color = text_color;
  style.background_color = bg_color;
  return style;
}

TEST(DrawCaptionTextTest, EmptyTextReturnsError) {
  CpuFrame frame(100, 100);
  CaptionStyle style = make_style(24.0);
  const auto result = draw_caption_text(frame, style, "", 10);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), CaptionBurnInError::EmptyText);
}

TEST(DrawCaptionTextTest, InvalidFontSizeReturnsError) {
  CpuFrame frame(100, 100);
  CaptionStyle style = make_style(0.0);
  const auto result = draw_caption_text(frame, style, "HELLO", 10);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), CaptionBurnInError::InvalidFontSize);
}

TEST(DrawCaptionTextTest, FrameTooSmallReturnsError) {
  CpuFrame frame(1, 1);
  CaptionStyle style = make_style(24.0);
  const auto result = draw_caption_text(frame, style, "HELLO", 10);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), CaptionBurnInError::FrameTooSmall);
}

TEST(DrawCaptionTextTest, DrawsHelloOn100x100Frame) {
  CpuFrame frame(100, 100);
  frame.clear(0.0, 0.0, 0.0, 1.0); // Black background

  CaptionStyle style = make_style(24.0);
  const auto result = draw_caption_text(frame, style, "HELLO", 10);

  EXPECT_FALSE(result.has_value());

  // Check that some pixels were modified (text was rendered)
  bool has_non_black_pixels = false;
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      if (pixel[0] > 0.01f || pixel[1] > 0.01f || pixel[2] > 0.01f) {
        has_non_black_pixels = true;
        break;
      }
    }
    if (has_non_black_pixels)
      break;
  }
  EXPECT_TRUE(has_non_black_pixels);
}

TEST(DrawCaptionTextTest, DrawsMultiLineText) {
  CpuFrame frame(200, 100);
  frame.clear(0.0, 0.0, 0.0, 1.0);

  CaptionStyle style = make_style(24.0);
  const auto result = draw_caption_text(frame, style, "HELLO\nWORLD", 10);

  EXPECT_FALSE(result.has_value());

  // Check that text was rendered
  bool has_non_black_pixels = false;
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      if (pixel[0] > 0.01f || pixel[1] > 0.01f || pixel[2] > 0.01f) {
        has_non_black_pixels = true;
        break;
      }
    }
    if (has_non_black_pixels)
      break;
  }
  EXPECT_TRUE(has_non_black_pixels);
}

TEST(DrawCaptionTextTest, UnsupportedGlyphsRenderReplacement) {
  CpuFrame frame(100, 100);
  frame.clear(0.0, 0.0, 0.0, 1.0);

  CaptionStyle style = make_style(24.0);
  // Copyright symbol (©) is not in the supported glyph set
  const auto result = draw_caption_text(frame, style, "Copyright © 2024", 10);

  EXPECT_FALSE(result.has_value());

  // Should still render something (replacement glyphs)
  bool has_non_black_pixels = false;
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      if (pixel[0] > 0.01f || pixel[1] > 0.01f || pixel[2] > 0.01f) {
        has_non_black_pixels = true;
        break;
      }
    }
    if (has_non_black_pixels)
      break;
  }
  EXPECT_TRUE(has_non_black_pixels);
}

TEST(BurnInCaptionsTest, DrawsCaptionWhenTimeInRange) {
  CpuFrame frame(200, 100);
  frame.clear(0.0, 0.0, 0.0, 1.0);

  std::vector<Caption> captions;
  captions.push_back(make_caption(0, 1, 10, 1, "TEST", make_style(24.0)));

  // Time 5 is within range [0, 10)
  const auto result = burn_in_captions(frame, captions, Time(5, 1));

  EXPECT_FALSE(result.has_value());

  // Check that text was rendered
  bool has_non_black_pixels = false;
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      if (pixel[0] > 0.01f || pixel[1] > 0.01f || pixel[2] > 0.01f) {
        has_non_black_pixels = true;
        break;
      }
    }
    if (has_non_black_pixels)
      break;
  }
  EXPECT_TRUE(has_non_black_pixels);
}

TEST(BurnInCaptionsTest, SkipsCaptionWhenTimeOutOfRange) {
  CpuFrame frame(200, 100);
  frame.clear(0.0, 0.0, 0.0, 1.0);

  // Get initial pixel state
  std::vector<float> initial_pixels;
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      initial_pixels.push_back(pixel[0]);
      initial_pixels.push_back(pixel[1]);
      initial_pixels.push_back(pixel[2]);
      initial_pixels.push_back(pixel[3]);
    }
  }

  std::vector<Caption> captions;
  captions.push_back(make_caption(10, 1, 10, 1, "TEST", make_style(24.0)));

  // Time 5 is NOT within range [10, 20)
  const auto result = burn_in_captions(frame, captions, Time(5, 1));

  EXPECT_FALSE(result.has_value());

  // Frame should be unchanged
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      EXPECT_FLOAT_EQ(pixel[0], 0.0f);
      EXPECT_FLOAT_EQ(pixel[1], 0.0f);
      EXPECT_FLOAT_EQ(pixel[2], 0.0f);
      EXPECT_FLOAT_EQ(pixel[3], 1.0f);
    }
  }
}

TEST(BurnInCaptionsTest, SkipsEmptyTextCaptionsSilently) {
  CpuFrame frame(200, 100);
  frame.clear(0.0, 0.0, 0.0, 1.0);

  std::vector<Caption> captions;
  // Empty text caption
  captions.push_back(make_caption(0, 1, 10, 1, "", make_style(24.0)));
  // Valid caption
  captions.push_back(make_caption(0, 1, 10, 1, "TEST", make_style(24.0)));

  const auto result = burn_in_captions(frame, captions, Time(5, 1));

  EXPECT_FALSE(result.has_value());

  // Check that text from valid caption was rendered
  bool has_non_black_pixels = false;
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      if (pixel[0] > 0.01f || pixel[1] > 0.01f || pixel[2] > 0.01f) {
        has_non_black_pixels = true;
        break;
      }
    }
    if (has_non_black_pixels)
      break;
  }
  EXPECT_TRUE(has_non_black_pixels);
}

TEST(WriteCaptionSidecarTest, WritesSrtFile) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  std::filesystem::path media_path = temp_dir / "test_video.mp4";

  std::vector<Caption> captions;
  captions.push_back(make_caption(0, 1000, 2000, 1000, "Hello World", make_style(24.0)));

  const auto result =
      write_caption_sidecar(captions, media_path, SidecarFormat::Srt, Time(0, 1), Time(10, 1));

  ASSERT_TRUE(result) << "write_caption_sidecar should succeed";
  EXPECT_TRUE(result.value().written);
  EXPECT_EQ(result.value().cue_count, 1u);
  EXPECT_EQ(result.value().path.extension(), ".srt");

  // Read and verify the file content
  std::ifstream file(result.value().path);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  EXPECT_NE(content.find("Hello World"), std::string::npos);
  EXPECT_NE(content.find("00:00:00,000"), std::string::npos);
}

TEST(WriteCaptionSidecarTest, WritesWebVttFile) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  std::filesystem::path media_path = temp_dir / "test_video2.mp4";

  std::vector<Caption> captions;
  captions.push_back(make_caption(0, 1000, 2000, 1000, "Hello World", make_style(24.0)));

  const auto result =
      write_caption_sidecar(captions, media_path, SidecarFormat::WebVtt, Time(0, 1), Time(10, 1));

  ASSERT_TRUE(result) << "write_caption_sidecar should succeed";
  EXPECT_TRUE(result.value().written);
  EXPECT_EQ(result.value().cue_count, 1u);
  EXPECT_EQ(result.value().path.extension(), ".vtt");

  // Read and verify the file content
  std::ifstream file(result.value().path);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  EXPECT_EQ(content.find("WEBVTT"), 0u);
  EXPECT_NE(content.find("Hello World"), std::string::npos);
}

TEST(WriteCaptionSidecarTest, NoCaptionsReturnsError) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  std::filesystem::path media_path = temp_dir / "test_video3.mp4";

  std::vector<Caption> captions;

  const auto result =
      write_caption_sidecar(captions, media_path, SidecarFormat::Srt, Time(0, 1), Time(10, 1));

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CaptionSidecarError::NoCaptions);
}

TEST(WriteCaptionSidecarTest, FiltersByTimeRange) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
  std::filesystem::path media_path = temp_dir / "test_video4.mp4";

  std::vector<Caption> captions;
  // Caption outside the requested range
  captions.push_back(make_caption(20, 1, 2, 1, "Outside Range", make_style(24.0)));
  // Caption inside the requested range
  captions.push_back(make_caption(0, 1000, 2000, 1000, "Inside Range", make_style(24.0)));

  // Request only time 0-10, which should only include the second caption
  const auto result =
      write_caption_sidecar(captions, media_path, SidecarFormat::Srt, Time(0, 1), Time(10, 1));

  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().cue_count, 1u);

  std::ifstream file(result.value().path);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  EXPECT_NE(content.find("Inside Range"), std::string::npos);
  EXPECT_EQ(content.find("Outside Range"), std::string::npos);
}

} // namespace
} // namespace video_editor::export_service