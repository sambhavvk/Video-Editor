// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/probe.h"
#include "video_editor/media_codec/runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace video_editor::media {
namespace {

void write_u16(std::ostream& output, const std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                  static_cast<char>((value >> 8U) & 0xFFU)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ostream& output, const std::uint32_t value) {
  const std::array<char, 4> bytes{static_cast<char>(value & 0xFFU),
                                  static_cast<char>((value >> 8U) & 0xFFU),
                                  static_cast<char>((value >> 16U) & 0xFFU),
                                  static_cast<char>((value >> 24U) & 0xFFU)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path write_test_wave() {
  const auto path = std::filesystem::temp_directory_path() / "video_editor_probe_test.wav";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  constexpr std::uint32_t sample_rate = 48'000;
  constexpr std::uint16_t channels = 2;
  constexpr std::uint16_t bits = 16;
  constexpr std::uint32_t frames = 480;
  constexpr std::uint32_t data_size = frames * channels * (bits / 8U);

  output.write("RIFF", 4);
  write_u32(output, 36U + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16);
  write_u16(output, 1);
  write_u16(output, channels);
  write_u32(output, sample_rate);
  write_u32(output, sample_rate * channels * (bits / 8U));
  write_u16(output, channels * (bits / 8U));
  write_u16(output, bits);
  output.write("data", 4);
  write_u32(output, data_size);
  const std::array<char, data_size> silence{};
  output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
  return path;
}

TEST(MediaRuntime, ReportsPinnedAbi) {
  const RuntimeInfo info = runtime_info();
  EXPECT_TRUE(info.expected_abi);
  EXPECT_EQ(info.avformat.major, 62U);
  EXPECT_EQ(info.avcodec.major, 62U);
  EXPECT_FALSE(info.configuration.empty());
  EXPECT_FALSE(info.license.empty());
}

TEST(MediaProbe, RejectsMissingFileWithoutThrowing) {
  const auto result = probe("/definitely/not/a/video-editor-test-file.mov");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, MediaErrorCode::FileNotFound);
}

TEST(MediaProbe, DescribesEveryWaveStream) {
  const auto path = write_test_wave();
  const auto result = probe(path);
  std::filesystem::remove(path);

  ASSERT_TRUE(result) << result.error().message;
  const AssetDescriptor& asset = result.value();
  ASSERT_EQ(asset.streams.size(), 1U);
  EXPECT_EQ(asset.best_audio_stream, 0);
  EXPECT_EQ(asset.best_video_stream, -1);
  EXPECT_EQ(asset.streams.front().kind, StreamKind::Audio);
  ASSERT_TRUE(asset.streams.front().audio.has_value());
  EXPECT_EQ(asset.streams.front().audio->sample_rate, 48'000);
  EXPECT_EQ(asset.streams.front().audio->channels, 2);
  EXPECT_TRUE(asset.duration_microseconds.has_value());
}

} // namespace
} // namespace video_editor::media

