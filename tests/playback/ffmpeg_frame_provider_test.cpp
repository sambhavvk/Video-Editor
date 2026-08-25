// SPDX-License-Identifier: MPL-2.0
#include "video_editor/playback/asset_registry.h"
#include "video_editor/playback/ffmpeg_frame_provider.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace video_editor::playback {
namespace {

[[nodiscard]] std::string shell_quote(const std::filesystem::path& path) {
  const std::string value = path.string();
#ifdef _WIN32
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '"') {
      quoted += "\"\"";
    } else {
      quoted += character;
    }
  }
  quoted += '"';
  return quoted;
#else
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
#endif
}

void write_ppm(const std::filesystem::path& path, const std::array<std::uint8_t, 3>& color) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create playback test image");
  }
  output << "P6\n16 16\n255\n";
  for (int pixel = 0; pixel < 16 * 16; ++pixel) {
    output.write(reinterpret_cast<const char*>(color.data()),
                 static_cast<std::streamsize>(color.size()));
  }
  if (!output) {
    throw std::runtime_error("cannot write playback test image");
  }
}

class DeterministicVideo final {
public:
  DeterministicVideo() {
    directory_ = std::filesystem::temp_directory_path() /
                 ("video_editor_playback_" + edit::EntityId::generate().toString());
    std::filesystem::create_directories(directory_);
    constexpr std::array<std::array<std::uint8_t, 3>, 6> colors{{{255U, 0U, 0U},
                                                                 {255U, 0U, 0U},
                                                                 {0U, 255U, 0U},
                                                                 {0U, 255U, 0U},
                                                                 {0U, 0U, 255U},
                                                                 {0U, 0U, 255U}}};
    for (std::size_t index = 0; index < colors.size(); ++index) {
      std::array<char, 32> filename{};
      const int written = std::snprintf(filename.data(), filename.size(), "frame%03zu.ppm", index);
      if (written <= 0 || static_cast<std::size_t>(written) >= filename.size()) {
        throw std::runtime_error("cannot format playback test filename");
      }
      write_ppm(directory_ / filename.data(), colors[index]);
    }

    video_path_ = directory_ / "fixture.mkv";
    const auto input_pattern = directory_ / "frame%03d.ppm";
    const std::string command =
        shell_quote(VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -framerate 4 -i " + shell_quote(input_pattern) +
        " -frames:v 6 -c:v mpeg4 -g 6 -bf 2 -q:v 2 -pix_fmt yuv420p " + shell_quote(video_path_);
    if (std::system(command.c_str()) != 0 || !std::filesystem::is_regular_file(video_path_)) {
      throw std::runtime_error("ffmpeg could not create playback test video");
    }
  }

  ~DeterministicVideo() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  DeterministicVideo(const DeterministicVideo&) = delete;
  DeterministicVideo& operator=(const DeterministicVideo&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return video_path_;
  }

private:
  std::filesystem::path directory_;
  std::filesystem::path video_path_;
};

[[nodiscard]] const DeterministicVideo& fixture() {
  static const DeterministicVideo video;
  return video;
}

[[nodiscard]] render::AssetFrameRequest make_request(const edit::EntityId asset_id,
                                                     const edit::Time source_time,
                                                     const std::uint64_t epoch,
                                                     const bool permit_proxy = false) {
  return {.asset_id = asset_id,
          .source_time = source_time,
          .preferred_width = 16,
          .preferred_height = 16,
          .permit_proxy = permit_proxy,
          .request_epoch = epoch};
}

void expect_dominant_color(const render::CpuFrame& frame, const std::size_t channel) {
  ASSERT_EQ(frame.width(), 16);
  ASSERT_EQ(frame.height(), 16);
  const auto pixel = frame.pixel(8, 8);
  // MPEG-4 4:2:0 plus the documented Rec.709 inverse transfer keeps the
  // channel ordering deterministic without promising lossless amplitudes.
  EXPECT_GT(pixel[channel], 0.65F);
  for (std::size_t other = 0; other < 3U; ++other) {
    if (other != channel) {
      EXPECT_LT(pixel[other], 0.08F);
    }
  }
  EXPECT_NEAR(pixel[3], 1.0F, 0.001F);
}

TEST(AssetRegistry, SelectsPresentProxyAndFallsBackWhenItIsMissing) {
  AssetRegistry registry;
  const edit::EntityId asset_id = edit::EntityId::generate();
  EXPECT_FALSE(registry.register_asset({}, {{fixture().path(), 0}, std::nullopt, std::nullopt}));
  EXPECT_TRUE(registry.register_asset(
      asset_id, {{fixture().path(), 0}, AssetStreamLocation{fixture().path(), 0}, std::nullopt}));
  EXPECT_EQ(registry.size(), 1U);

  const auto without_map = registry.resolve(asset_id, true);
  ASSERT_TRUE(without_map.has_value());
  EXPECT_FALSE(without_map->is_proxy);

  const auto original = registry.resolve(asset_id, false);
  ASSERT_TRUE(original.has_value());
  EXPECT_FALSE(original->is_proxy);
  EXPECT_EQ(original->location.path, fixture().path());

  EXPECT_TRUE(registry.register_asset(
      asset_id, {{fixture().path(), 0},
                 AssetStreamLocation{fixture().path().parent_path() / "missing.mkv", 0},
                 std::nullopt}));
  const auto fallback = registry.resolve(asset_id, true);
  ASSERT_TRUE(fallback.has_value());
  EXPECT_FALSE(fallback->is_proxy);
  EXPECT_GT(fallback->registry_generation, original->registry_generation);
  EXPECT_GT(registry.generation(), fallback->registry_generation);
  EXPECT_TRUE(registry.unregister_asset(asset_id));
  EXPECT_FALSE(registry.resolve(asset_id, false).has_value());
}

TEST(FfmpegFrameProvider, SeeksExactlyAndUsesSequentialDecodeForNearbyFrames) {
  auto registry = std::make_shared<AssetRegistry>();
  const edit::EntityId asset_id = edit::EntityId::generate();
  ASSERT_TRUE(registry->register_asset(asset_id, {{fixture().path(), 0}, std::nullopt, std::nullopt}));
  FfmpegFrameProvider provider(registry);
  provider.begin_epoch(7);

  const auto first = provider.request_with_timing(make_request(asset_id, edit::Time(1, 8), 7));
  ASSERT_TRUE(first) << first.error->message;
  EXPECT_EQ(first.value->presentation.start, edit::Time(0, 1));
  EXPECT_EQ(first.value->presentation.duration, edit::Time(1, 4));
  expect_dominant_color(*first.value->pixels, 0U);
  const PlaybackStatistics after_first = provider.statistics();
  EXPECT_EQ(after_first.sessions_opened, 1U);
  EXPECT_EQ(after_first.seeks, 1U);

  const auto green = provider.request_with_timing(make_request(asset_id, edit::Time(5, 8), 7));
  ASSERT_TRUE(green) << green.error->message;
  EXPECT_EQ(green.value->presentation.start, edit::Time(1, 2));
  EXPECT_EQ(green.value->presentation.duration, edit::Time(1, 4));
  expect_dominant_color(*green.value->pixels, 1U);
  const PlaybackStatistics after_green = provider.statistics();
  EXPECT_EQ(after_green.seeks, after_first.seeks);
  EXPECT_EQ(after_green.sequential_requests, 1U);

  const auto blue = provider.request_with_timing(make_request(asset_id, edit::Time(9, 8), 7));
  ASSERT_TRUE(blue) << blue.error->message;
  EXPECT_EQ(blue.value->presentation.start, edit::Time(1, 1));
  EXPECT_EQ(blue.value->presentation.duration, edit::Time(1, 4));
  expect_dominant_color(*blue.value->pixels, 2U);

  const auto backward = provider.request_with_timing(make_request(asset_id, edit::Time(3, 8), 7));
  ASSERT_TRUE(backward) << backward.error->message;
  EXPECT_EQ(backward.value->presentation.start, edit::Time(1, 4));
  expect_dominant_color(*backward.value->pixels, 0U);
  EXPECT_EQ(provider.statistics().seeks, after_first.seeks + 1U);
}

TEST(FfmpegFrameProvider, UsesHalfOpenIntervalsAtAnExactFrameBoundary) {
  auto registry = std::make_shared<AssetRegistry>();
  const edit::EntityId asset_id = edit::EntityId::generate();
  ASSERT_TRUE(registry->register_asset(asset_id, {{fixture().path(), 0}, std::nullopt, std::nullopt}));
  FfmpegFrameProvider provider(registry);
  provider.begin_epoch(21);

  const auto boundary = provider.request_with_timing(make_request(asset_id, edit::Time(1, 2), 21));
  ASSERT_TRUE(boundary) << boundary.error->message;
  EXPECT_EQ(boundary.value->presentation.start, edit::Time(1, 2));
  expect_dominant_color(*boundary.value->pixels, 1U);

  const auto cached = provider.request_with_timing(make_request(asset_id, edit::Time(9, 16), 21));
  ASSERT_TRUE(cached) << cached.error->message;
  EXPECT_EQ(provider.statistics().cached_frame_requests, 1U);
}

TEST(FfmpegFrameProvider, RejectsStaleEpochBeforeOpeningMedia) {
  auto registry = std::make_shared<AssetRegistry>();
  const edit::EntityId asset_id = edit::EntityId::generate();
  ASSERT_TRUE(registry->register_asset(asset_id, {{fixture().path(), 0}, std::nullopt, std::nullopt}));
  FfmpegFrameProvider provider(registry);
  provider.begin_epoch(101);

  const auto stale_request =
      provider.request_with_timing(make_request(asset_id, edit::Time(0, 1), 100));
  ASSERT_FALSE(stale_request);
  EXPECT_EQ(stale_request.error->code, render::RenderErrorCode::StaleRequest);
  EXPECT_EQ(provider.statistics().sessions_opened, 0U);
}

TEST(FfmpegFrameProvider, HonorsProxyPolicyAndExplicitSessionInvalidation) {
  auto registry = std::make_shared<AssetRegistry>();
  const edit::EntityId asset_id = edit::EntityId::generate();
  ASSERT_TRUE(registry->register_asset(
      asset_id, {{fixture().path(), 0}, AssetStreamLocation{fixture().path(), 0}, std::nullopt}));
  FfmpegFrameProvider provider(registry);
  provider.begin_epoch(33);

  const auto proxy =
      provider.request_with_timing(make_request(asset_id, edit::Time(1, 8), 33, true));
  ASSERT_TRUE(proxy) << proxy.error->message;
  EXPECT_FALSE(proxy.value->used_proxy);

  const auto original =
      provider.request_with_timing(make_request(asset_id, edit::Time(1, 8), 33, false));
  ASSERT_TRUE(original) << original.error->message;
  EXPECT_FALSE(original.value->used_proxy);
  EXPECT_EQ(provider.statistics().sessions_opened, 1U);

  provider.invalidate(asset_id);
  const auto reopened =
      provider.request_with_timing(make_request(asset_id, edit::Time(1, 8), 33, false));
  ASSERT_TRUE(reopened) << reopened.error->message;
  EXPECT_EQ(provider.statistics().sessions_opened, 2U);
}

TEST(FfmpegFrameProvider, DiscardsFailedRecoveryBeforeTheNextRequest) {
  auto registry = std::make_shared<AssetRegistry>();
  const edit::EntityId asset_id = edit::EntityId::generate();
  ASSERT_TRUE(registry->register_asset(asset_id, {{fixture().path(), 0}, std::nullopt, std::nullopt}));
  FfmpegFrameProvider provider(registry);
  provider.begin_epoch(45);

  auto unsafe_request = make_request(asset_id, edit::Time(1, 8), 45);
  unsafe_request.preferred_width = 40'000;
  unsafe_request.preferred_height = 40'000;
  const auto failed = provider.request_with_timing(unsafe_request);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error->code, render::RenderErrorCode::ProviderFailure);
  EXPECT_EQ(provider.statistics().sessions_reopened, 1U);
  EXPECT_EQ(provider.statistics().sessions_opened, 2U);

  const auto recovered = provider.request_with_timing(make_request(asset_id, edit::Time(1, 8), 45));
  ASSERT_TRUE(recovered) << recovered.error->message;
  expect_dominant_color(*recovered.value->pixels, 0U);
  EXPECT_EQ(provider.statistics().sessions_opened, 3U);
}

} // namespace
} // namespace video_editor::playback
