// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/embedded_extract.h"

#include "video_editor/media_codec/probe.h"
#include "video_editor/media_codec/subtitle_extract.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {
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

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() / ("video_editor_embedded_caption_" +
                                                       std::to_string(++sequence_));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
  inline static std::uint64_t sequence_{0};
};

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
        << "00:00:00,500 --> 00:00:01,750\n"
        << "Embedded import cue\n\n";
  }

  std::ostringstream command_stream;
  command_stream << shell_quote(VIDEO_EDITOR_TEST_FFMPEG)
          << " -hide_banner -loglevel error -y"
          << " -f lavfi -i color=c=blue:s=320x240:d=2"
          << " -f lavfi -i sine=frequency=440:duration=2"
          << " -i " << shell_quote(srt_path.string())
          << " -map 0:v:0 -map 1:a:0 -map 2:0"
          << " -c:v libx264 -pix_fmt yuv420p -c:a aac -c:s srt -t 2 "
          << shell_quote(fixture_path.string());
  const std::string command = command_stream.str();
  return std::system(command.c_str()) == 0;
#endif
}
} // namespace

TEST(EmbeddedCaptionImport, RoundTripsToEditCaptionsWithProvenance) {
#ifndef VIDEO_EDITOR_TEST_FFMPEG
  GTEST_SKIP() << "ffmpeg is not available";
#else
  TemporaryDirectory directory;
  const std::filesystem::path fixture = directory.path() / "embedded.mkv";
  ASSERT_TRUE(create_subrip_fixture(fixture));

  const auto probed = video_editor::media::probe(fixture);
  ASSERT_TRUE(probed);
  const auto streams = video_editor::media::list_subtitle_streams(probed.value());
  ASSERT_EQ(streams.size(), 1U);

  constexpr char kAssetId[] = "asset-embedded-01";
  const auto imported = video_editor::caption_service::importEmbeddedSubtitles(
      fixture, probed.value(),
      {.asset_id = kAssetId, .stream_index = streams.front().index});
  ASSERT_TRUE(imported) << imported.error().message;

  const auto captions =
      video_editor::caption_service::toEditCaptionsFromEmbedded(imported.value());
  ASSERT_EQ(captions.size(), 1U);
  EXPECT_EQ(captions.front().text, "Embedded import cue");
  EXPECT_EQ(captions.front().provenance.source, video_editor::edit::CaptionWordSource::Imported);
  EXPECT_EQ(captions.front().provenance.model_identity,
            video_editor::caption_service::embeddedCaptionProvenance(kAssetId,
                                                                     streams.front().index));
  EXPECT_FALSE(captions.front().range.empty());
#endif
}

TEST(EmbeddedCaptionImport, RejectsUnsupportedSubtitleCodec) {
  video_editor::media::AssetDescriptor asset;
  video_editor::media::StreamDescriptor stream;
  stream.index = 1;
  stream.kind = video_editor::media::StreamKind::Subtitle;
  stream.codec_name = "ass";
  asset.streams.push_back(stream);

  const auto imported = video_editor::caption_service::importEmbeddedSubtitles(
      std::filesystem::path("/tmp/unused.mkv"), asset,
      {.asset_id = "asset-01", .stream_index = 1});
  ASSERT_FALSE(imported);
  EXPECT_EQ(imported.error().code, video_editor::media::MediaErrorCode::Unsupported);
}
