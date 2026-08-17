// SPDX-License-Identifier: MPL-2.0
//
// FFmpeg-dependent thumbnail and waveform generation against a small lavfi
// fixture. Pure resolver tests remain in thumbnail_service_test.cpp and
// waveform_service_test.cpp.

#include "video_editor/media_cache/cache_store.h"
#include "video_editor/media_cache/thumbnail_service.h"
#include "video_editor/media_cache/waveform_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::media_cache {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("video_editor_media_cache_ffmpeg_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
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

[[nodiscard]] bool create_av_fixture(const std::filesystem::path& path) {
#ifndef VIDEO_EDITOR_TEST_FFMPEG
  static_cast<void>(path);
  return false;
#else
  const std::vector<std::string> arguments{
      VIDEO_EDITOR_TEST_FFMPEG,
      "-hide_banner",
      "-loglevel",
      "error",
      "-nostdin",
      "-y",
      "-f",
      "lavfi",
      "-i",
      "testsrc2=size=320x180:rate=25:duration=1",
      "-f",
      "lavfi",
      "-i",
      "sine=frequency=440:sample_rate=48000:duration=1",
      "-c:v",
      "mpeg4",
      "-q:v",
      "8",
      "-c:a",
      "pcm_s16le",
      "-shortest",
      path.string(),
  };
  std::ostringstream command;
  for (const std::string& argument : arguments) {
    if (command.tellp() > 0) {
      command << ' ';
    }
    command << shell_quote(argument);
  }
  return std::system(command.str().c_str()) == 0;
#endif
}

} // namespace

TEST(MediaCacheFfmpeg, GeneratesAndReloadsThumbnailAndWaveform) {
#ifndef VIDEO_EDITOR_TEST_FFMPEG
  GTEST_SKIP() << "ffmpeg is not available";
#else
  TemporaryDirectory directory;
  const std::filesystem::path fixture = directory.path() / "fixture.mov";
  ASSERT_TRUE(create_av_fixture(fixture)) << "failed to create ffmpeg fixture";

  CacheStore store(directory.path() / "cache");
  const std::string asset_id = "ffmpeg-asset";

  ThumbnailOptions thumb_options;
  thumb_options.maximum_width = 160;
  thumb_options.strategy = ThumbnailOptions::Strategy::Middle;
  const auto thumbnail = generate_thumbnail(fixture, -1, thumb_options, asset_id, store);
  ASSERT_TRUE(thumbnail) << thumbnail.error().message;
  EXPECT_FALSE(thumbnail.value().jpeg_bytes.empty());
  EXPECT_GT(thumbnail.value().width, 0);
  EXPECT_GT(thumbnail.value().height, 0);

  const auto loaded_thumb = load_thumbnail(asset_id, thumb_options, store);
  ASSERT_TRUE(loaded_thumb) << loaded_thumb.error().message;
  EXPECT_EQ(loaded_thumb.value().jpeg_bytes, thumbnail.value().jpeg_bytes);

  WaveformOptions wave_options;
  wave_options.finest_level_buckets = 64;
  wave_options.level_count = 4;
  const auto waveform = generate_waveform(fixture, -1, wave_options, asset_id, store);
  ASSERT_TRUE(waveform) << waveform.error().message;
  EXPECT_GT(waveform.value().total_samples, 0);
  ASSERT_FALSE(waveform.value().levels.empty());
  EXPECT_GT(waveform.value().levels.front().bucket_count, 0);

  const auto loaded_wave = load_waveform(asset_id, wave_options, store);
  ASSERT_TRUE(loaded_wave) << loaded_wave.error().message;
  EXPECT_EQ(loaded_wave.value().total_samples, waveform.value().total_samples);
  EXPECT_EQ(loaded_wave.value().levels.size(), waveform.value().levels.size());
#endif
}

} // namespace video_editor::media_cache
