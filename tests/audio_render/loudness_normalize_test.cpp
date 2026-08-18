// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/loudness_normalize.h"
#include "video_editor/audio_render/original_audio_registry.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace video_editor::audio_render {
namespace {

constexpr std::size_t kFixtureFrames = 144'000;

void write_u16(std::ofstream& output, const std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value & 0xffU),
                                  static_cast<char>((value >> 8U) & 0xffU)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ofstream& output, const std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value & 0xffU), static_cast<char>((value >> 8U) & 0xffU),
      static_cast<char>((value >> 16U) & 0xffU), static_cast<char>((value >> 24U) & 0xffU)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::filesystem::path make_constant_wav() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("video_editor_loudness_normalize_" + edit::EntityId::generate().toString() +
                     ".wav");
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("could not create loudness test WAV");
  }

  constexpr std::uint16_t amplitude = 1'024;
  const auto data_size = static_cast<std::uint32_t>(kFixtureFrames * 4U);
  output.write("RIFF", 4);
  write_u32(output, 36U + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16U);
  write_u16(output, 1U);
  write_u16(output, 2U);
  write_u32(output, kTimelineAudioSampleRate);
  write_u32(output, kTimelineAudioSampleRate * 4U);
  write_u16(output, 4U);
  write_u16(output, 16U);
  output.write("data", 4);
  write_u32(output, data_size);
  for (std::size_t frame = 0; frame < kFixtureFrames; ++frame) {
    write_u16(output, amplitude);
    write_u16(output, amplitude);
  }
  if (!output) {
    throw std::runtime_error("could not finish loudness test WAV");
  }
  return path;
}

class LoudnessFixture final {
public:
  LoudnessFixture() : wav_path_(make_constant_wav()) {
    edit::Asset asset;
    asset.name = "constant.wav";
    asset.duration = edit::Time(kFixtureFrames, kTimelineAudioSampleRate);
    asset.has_audio = true;
    asset.audio_sample_rate = kTimelineAudioSampleRate;
    asset.audio_channels = kTimelineAudioChannels;
    asset_id_ = asset.id;

    edit::Sequence sequence;
    sequence.name = "Main";
    sequence.audio_sample_rate = kTimelineAudioSampleRate;
    sequence_id_ = sequence.id;
    project_.assets.push_back(asset);
    project_.sequences.push_back(sequence);
  }

  ~LoudnessFixture() {
    std::error_code error;
    std::filesystem::remove(wav_path_, error);
  }

  LoudnessFixture(const LoudnessFixture&) = delete;
  LoudnessFixture& operator=(const LoudnessFixture&) = delete;

  [[nodiscard]] edit::TimelineSnapshot snapshot(const bool muted = false) const {
    edit::Clip clip;
    clip.asset_id = asset_id_;
    clip.kind = edit::ClipKind::Audio;
    clip.name = "Audio";
    clip.timeline_range = edit::TimeRange(edit::Time(0, kTimelineAudioSampleRate),
                                          edit::Time(kFixtureFrames, kTimelineAudioSampleRate));
    clip.source_range = clip.timeline_range;

    edit::Track track;
    track.kind = edit::TrackKind::Audio;
    track.name = "Audio";
    track.muted = muted;
    track.clips.push_back(clip);

    edit::Project project = project_;
    project.sequences.front().tracks.push_back(std::move(track));
    edit::TimelineEditor editor(std::move(project));
    const auto result = editor.snapshot(sequence_id_, edit::Revision{0});
    if (!result) {
      throw std::runtime_error(result.error().message);
    }
    return result.value();
  }

  [[nodiscard]] std::shared_ptr<const OriginalAudioProvider> originals() const {
    auto registry = std::make_shared<OriginalAudioRegistry>();
    if (!registry->register_original(asset_id_, {wav_path_, -1})) {
      throw std::runtime_error("could not register loudness test WAV");
    }
    return registry;
  }

private:
  std::filesystem::path wav_path_;
  edit::Project project_;
  edit::EntityId asset_id_;
  edit::EntityId sequence_id_;
};

TEST(LoudnessNormalize, ReturnsZeroGainForSilentTimeline) {
  const LoudnessFixture fixture;
  const auto result = compute_normalization_gain(fixture.snapshot(true), fixture.originals());

  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("silent"), std::string::npos);
}

TEST(LoudnessNormalize, ComputesPositiveGainForQuietTimeline) {
  const LoudnessFixture fixture;
  const auto result = compute_normalization_gain(fixture.snapshot(), fixture.originals());

  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(std::isfinite(result.value().integrated_lufs));
  EXPECT_GT(result.value().gain_db, 0.0);
  EXPECT_NEAR(result.value().integrated_lufs + result.value().gain_db, -23.0, 1e-12);
}

TEST(LoudnessNormalize, RespectsCustomTargetLufs) {
  const LoudnessFixture fixture;
  const auto broadcast = compute_normalization_gain(fixture.snapshot(), fixture.originals(), -23.0);
  const auto custom = compute_normalization_gain(fixture.snapshot(), fixture.originals(), -18.0);

  ASSERT_TRUE(broadcast) << broadcast.error().message;
  ASSERT_TRUE(custom) << custom.error().message;
  EXPECT_NEAR(custom.value().gain_db - broadcast.value().gain_db, 5.0, 1e-12);
}

TEST(LoudnessNormalize, HonorsCancellation) {
  const LoudnessFixture fixture;
  std::stop_source source;
  source.request_stop();
  const auto result =
      compute_normalization_gain(fixture.snapshot(), fixture.originals(), -23.0, source.get_token());

  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("cancelled"), std::string::npos);
}

} // namespace
} // namespace video_editor::audio_render
