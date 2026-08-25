// SPDX-License-Identifier: MPL-2.0
#include "video_editor/asset_service/asset_service.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace video_editor::assets {
namespace {

class TemporaryFile {
public:
  explicit TemporaryFile(const std::string& contents) {
    static int sequence = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("video-editor-fingerprint-" + std::to_string(++sequence) + ".bin");
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << contents;
  }
  ~TemporaryFile() { std::filesystem::remove(path_); }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

TEST(Fingerprint, StableForUnchangedFileAndDetectsContentChanges) {
  TemporaryFile first("creator-media-one");
  TemporaryFile identical("creator-media-one");
  TemporaryFile second("creator-media-two");
  const auto fingerprint_a = fingerprint_file(first.path(), true);
  const auto fingerprint_b = fingerprint_file(identical.path(), true);
  const auto fingerprint_c = fingerprint_file(second.path(), true);

  ASSERT_TRUE(fingerprint_a);
  ASSERT_TRUE(fingerprint_b);
  ASSERT_TRUE(fingerprint_c);
  EXPECT_TRUE(fingerprint_a.value().content_matches(fingerprint_b.value()));
  EXPECT_FALSE(fingerprint_a.value().content_matches(fingerprint_c.value()));
  EXPECT_EQ(fingerprint_a.value().full_sha256, fingerprint_b.value().full_sha256);
}

TEST(ProxyPolicy, RecommendsProxyOnlyForDifficultFourKVideo) {
  AssetRecord asset;
  media::StreamDescriptor stream;
  stream.codec_name = "h264";
  media::VideoDescription video;
  video.width = 3840;
  video.height = 2160;
  stream.video = video;
  asset.descriptor.streams.push_back(stream);
  EXPECT_TRUE(AssetService::should_recommend_proxy(asset));

  asset.descriptor.streams.front().video->width = 1920;
  asset.descriptor.streams.front().video->height = 1080;
  EXPECT_FALSE(AssetService::should_recommend_proxy(asset));
}

TEST(AssetService, ImportsNumberedStillSequenceAsOnePatternUri) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("video-editor-sequence-" + std::to_string(std::rand()));
  std::filesystem::create_directories(directory);
  const auto write_ppm = [&](const std::string& name) {
    std::ofstream output(directory / name, std::ios::binary | std::ios::trunc);
    output << "P6\n2 2\n255\n";
    const char pixel[] = {static_cast<char>(255), 0, 0};
    for (int i = 0; i < 4; ++i) {
      output.write(pixel, 3);
    }
  };
  write_ppm("shot0001.ppm");
  write_ppm("shot0002.ppm");
  write_ppm("shot0003.ppm");
  AssetService service;
  const auto imported = service.import(directory / "shot0001.ppm");
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  ASSERT_TRUE(imported) << imported.error().message;
  EXPECT_NE(imported.value().uri.string().find("%04d"), std::string::npos);
  EXPECT_EQ(imported.value().descriptor.format_name, "image2-sequence");
}

} // namespace
} // namespace video_editor::assets
