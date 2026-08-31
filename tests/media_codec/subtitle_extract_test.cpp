// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/probe.h"
#include "video_editor/media_codec/subtitle_extract.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace video_editor::media {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() / ("video_editor_subtitle_extract_" +
                                                       std::to_string(++sequence_));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
  inline static std::uint64_t sequence_{0};
};

[[nodiscard]] std::string shell_quote(const std::string_view value) {
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
  return quoted;
}

[[nodiscard]] bool run_command(const std::string& command) {
  return std::system(command.c_str()) == 0;
}

[[nodiscard]] bool create_subrip_fixture(const std::filesystem::path& fixture_path) {
#ifndef VIDEO_EDITOR_TEST_FFMPEG
  static_cast<void>(fixture_path);
  return false;
#else
  TemporaryDirectory directory;
  const std::filesystem::path srt_path = directory.path() / "captions.srt";
  {
    std::ofstream srt(srt_path);
    srt << "1\n"
        << "00:00:00,000 --> 00:00:01,000\n"
        << "First embedded cue\n\n"
        << "2\n"
        << "00:00:01,000 --> 00:00:02,500\n"
        << "Second embedded cue\n\n";
  }

  std::ostringstream command;
  command << shell_quote(VIDEO_EDITOR_TEST_FFMPEG)
          << " -hide_banner -loglevel error -y"
          << " -f lavfi -i color=c=blue:s=320x240:d=3"
          << " -f lavfi -i sine=frequency=440:duration=3"
          << " -i " << shell_quote(srt_path.string())
          << " -map 0:v:0 -map 1:a:0 -map 2:0"
          << " -c:v libx264 -pix_fmt yuv420p -c:a aac -c:s srt -t 3 "
          << shell_quote(fixture_path.string());
  return run_command(command.str());
#endif
}

} // namespace

TEST(SubtitleExtract, RecognizesSupportedTextCodecs) {
  EXPECT_TRUE(is_text_subtitle_codec("subrip"));
  EXPECT_TRUE(is_text_subtitle_codec("SRT"));
  EXPECT_TRUE(is_text_subtitle_codec("mov_text"));
  EXPECT_TRUE(is_text_subtitle_codec("webvtt"));
  EXPECT_FALSE(is_text_subtitle_codec("hdmv_pgs_subtitle"));
  EXPECT_TRUE(is_bitmap_subtitle_codec("ass"));
}

TEST(SubtitleExtract, ProbeAndExtractTextSubtitles) {
#ifndef VIDEO_EDITOR_TEST_FFMPEG
  GTEST_SKIP() << "ffmpeg is not available";
#else
  TemporaryDirectory directory;
  const std::filesystem::path fixture = directory.path() / "embedded_subtitles.mkv";
  ASSERT_TRUE(create_subrip_fixture(fixture)) << "failed to create subtitle fixture";

  const auto probed = probe(fixture);
  ASSERT_TRUE(probed) << probed.error().message;

  const auto streams = list_subtitle_streams(probed.value());
  ASSERT_EQ(streams.size(), 1U);
  EXPECT_EQ(streams.front().index, 2);
  EXPECT_TRUE(streams.front().supported_text_codec);
  EXPECT_EQ(streams.front().codec_name, "subrip");

  const auto extracted = extract_text_subtitles(fixture, streams.front().index);
  ASSERT_TRUE(extracted) << extracted.error().message;
  ASSERT_GE(extracted.value().cues.size(), 2U);
  EXPECT_EQ(extracted.value().cues.front().text, "First embedded cue");
  EXPECT_LT(extracted.value().cues.front().start_microseconds,
            extracted.value().cues.front().end_microseconds);
  EXPECT_EQ(extracted.value().cues.back().text, "Second embedded cue");
#endif
}

TEST(SubtitleExtract, RejectsUnsupportedCodecName) {
  media::AssetDescriptor asset;
  media::StreamDescriptor stream;
  stream.index = 0;
  stream.kind = StreamKind::Subtitle;
  stream.codec_name = "hdmv_pgs_subtitle";
  asset.streams.push_back(stream);

  const auto streams = list_subtitle_streams(asset);
  ASSERT_EQ(streams.size(), 1U);
  EXPECT_FALSE(streams.front().supported_text_codec);

  TemporaryDirectory directory;
  const std::filesystem::path missing = directory.path() / "missing.mkv";
  const auto extracted = extract_text_subtitles(missing, 0);
  ASSERT_FALSE(extracted);
  EXPECT_EQ(extracted.error().code, MediaErrorCode::FileNotFound);
}

} // namespace video_editor::media
