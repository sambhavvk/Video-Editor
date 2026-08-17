// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/caption_burn_in.h"

#include "video_editor/edit_model/model.h"
#include "video_editor/render_engine/frame.h"

#include <algorithm>
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

struct PixelBounds final {
  int left{0};
  int top{0};
  int right{-1};
  int bottom{-1};

  [[nodiscard]] bool empty() const noexcept {
    return right < left || bottom < top;
  }
};

PixelBounds non_black_bounds(const CpuFrame& frame) {
  PixelBounds bounds;
  for (int y = 0; y < frame.height(); ++y) {
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      if (pixel[0] <= 0.01F && pixel[1] <= 0.01F && pixel[2] <= 0.01F) {
        continue;
      }
      bounds.left = bounds.empty() ? x : std::min(bounds.left, x);
      bounds.top = bounds.empty() ? y : std::min(bounds.top, y);
      bounds.right = std::max(bounds.right, x);
      bounds.bottom = std::max(bounds.bottom, y);
    }
  }
  return bounds;
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

TEST(DrawCaptionTextTest, HonorsHorizontalAlignmentInsideSafeMargins) {
  const auto bounds_for = [](const CaptionAlignment alignment) {
    CpuFrame frame(100, 80);
    frame.clear(0.0, 0.0, 0.0, 1.0);
    CaptionStyle style = make_style(8.0, {1.0, 1.0, 1.0, 1.0}, {0.0, 0.0, 0.0, 0.0});
    style.alignment = alignment;
    style.safe_margin = 0.1;
    style.vertical_position = 0.5;
    EXPECT_FALSE(draw_caption_text(frame, style, "A", 10).has_value());
    return non_black_bounds(frame);
  };

  const PixelBounds left = bounds_for(CaptionAlignment::Left);
  const PixelBounds center = bounds_for(CaptionAlignment::Center);
  const PixelBounds right = bounds_for(CaptionAlignment::Right);
  ASSERT_FALSE(left.empty());
  ASSERT_FALSE(center.empty());
  ASSERT_FALSE(right.empty());
  EXPECT_EQ(left.left, 10);   // left safe edge
  EXPECT_EQ(center.left, 47); // six-pixel cell centered in [10, 90)
  EXPECT_EQ(right.left, 84);  // right edge at 90 px
}

TEST(DrawCaptionTextTest, HonorsNormalizedVerticalPosition) {
  const auto bounds_for = [](const double position) {
    CpuFrame frame(100, 80);
    frame.clear(0.0, 0.0, 0.0, 1.0);
    CaptionStyle style = make_style(8.0, {1.0, 1.0, 1.0, 1.0}, {0.0, 0.0, 0.0, 0.0});
    style.safe_margin = 0.1;
    style.vertical_position = position;
    EXPECT_FALSE(draw_caption_text(frame, style, "A", 10).has_value());
    return non_black_bounds(frame);
  };

  const PixelBounds upper = bounds_for(0.25);
  const PixelBounds lower = bounds_for(0.75);
  ASSERT_FALSE(upper.empty());
  ASSERT_FALSE(lower.empty());
  EXPECT_EQ(upper.top, 16); // safe top 8 + 25% of the 64 px safe height - 8 px cell
  EXPECT_EQ(lower.top, 48);
  EXPECT_EQ(upper.left, lower.left);
}

TEST(DrawCaptionTextTest, SafeMarginMovesAlignedTextAwayFromFrameEdge) {
  const auto bounds_for = [](const double margin) {
    CpuFrame frame(100, 80);
    frame.clear(0.0, 0.0, 0.0, 1.0);
    CaptionStyle style = make_style(8.0, {1.0, 1.0, 1.0, 1.0}, {0.0, 0.0, 0.0, 0.0});
    style.alignment = CaptionAlignment::Left;
    style.safe_margin = margin;
    style.vertical_position = 0.5;
    EXPECT_FALSE(draw_caption_text(frame, style, "A", 10).has_value());
    return non_black_bounds(frame);
  };

  EXPECT_EQ(bounds_for(0.0).left, 0);
  EXPECT_EQ(bounds_for(0.2).left, 20);
}

TEST(DrawCaptionTextTest, OutlineAddsDeterministicColoredPixelsAroundGlyph) {
  CpuFrame plain_frame(100, 80);
  CpuFrame outlined_frame(100, 80);
  plain_frame.clear(0.0, 0.0, 0.0, 1.0);
  outlined_frame.clear(0.0, 0.0, 0.0, 1.0);

  CaptionStyle plain = make_style(8.0, {1.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0, 0.0});
  plain.safe_margin = 0.1;
  plain.vertical_position = 0.5;
  CaptionStyle outlined = plain;
  outlined.outline_width = 2.0;
  outlined.outline_color = {0.0, 0.0, 1.0, 1.0};

  EXPECT_FALSE(draw_caption_text(plain_frame, plain, "A", 10).has_value());
  EXPECT_FALSE(draw_caption_text(outlined_frame, outlined, "A", 10).has_value());
  const PixelBounds plain_bounds = non_black_bounds(plain_frame);
  const PixelBounds outlined_bounds = non_black_bounds(outlined_frame);
  ASSERT_FALSE(plain_bounds.empty());
  ASSERT_FALSE(outlined_bounds.empty());
  EXPECT_LT(outlined_bounds.left, plain_bounds.left);
  EXPECT_LT(outlined_bounds.top, plain_bounds.top);

  bool found_blue_outline = false;
  for (int y = 0; y < outlined_frame.height(); ++y) {
    for (int x = 0; x < outlined_frame.width(); ++x) {
      const auto pixel = outlined_frame.pixel(x, y);
      if (pixel[2] > 0.5F && pixel[0] < 0.5F) {
        found_blue_outline = true;
        break;
      }
    }
    if (found_blue_outline) {
      break;
    }
  }
  EXPECT_TRUE(found_blue_outline);
}

TEST(DrawCaptionTextTest, DefaultStyleFieldsRemainAStableCenteredBottomLayout) {
  CpuFrame default_frame(100, 80);
  CpuFrame explicit_frame(100, 80);
  default_frame.clear(0.0, 0.0, 0.0, 1.0);
  explicit_frame.clear(0.0, 0.0, 0.0, 1.0);

  CaptionStyle default_style = make_style(8.0, {1.0, 1.0, 1.0, 1.0}, {0.0, 0.0, 0.0, 0.0});
  CaptionStyle explicit_style = default_style;
  explicit_style.alignment = CaptionAlignment::Center;
  explicit_style.vertical_position = 0.9;
  explicit_style.safe_margin = 0.05;
  explicit_style.outline_width = 0.0;
  explicit_style.outline_color = {0.0, 0.0, 0.0, 1.0};

  EXPECT_FALSE(draw_caption_text(default_frame, default_style, "A", 10).has_value());
  EXPECT_FALSE(draw_caption_text(explicit_frame, explicit_style, "A", 10).has_value());
  EXPECT_TRUE(std::equal(default_frame.pixels().begin(), default_frame.pixels().end(),
                         explicit_frame.pixels().begin(), explicit_frame.pixels().end()));
  const PixelBounds bounds = non_black_bounds(default_frame);
  ASSERT_FALSE(bounds.empty());
  EXPECT_EQ(bounds.left, 47);
  EXPECT_EQ(bounds.bottom, 68); // legacy bottom margin of 10 px is retained
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
