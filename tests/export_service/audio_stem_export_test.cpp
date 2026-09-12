// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/original_audio_registry.h"
#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/timeline_editor.h"
#include "video_editor/export_service/audio_stem_export.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace video_editor::export_service {
namespace {

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

void write_stereo_wav(const std::filesystem::path& path, const std::span<const std::int16_t> left) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("could not create stem-export test WAV");
  }
  const auto data_size = static_cast<std::uint32_t>(left.size() * 4U);
  output.write("RIFF", 4);
  write_u32(output, 36U + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16U);
  write_u16(output, 1U);
  write_u16(output, 2U);
  write_u32(output, audio_render::kTimelineAudioSampleRate);
  write_u32(output, audio_render::kTimelineAudioSampleRate * 4U);
  write_u16(output, 4U);
  write_u16(output, 16U);
  output.write("data", 4);
  write_u32(output, data_size);
  for (const auto sample : left) {
    write_u16(output, static_cast<std::uint16_t>(sample));
    write_u16(output, static_cast<std::uint16_t>(sample));
  }
  if (!output) {
    throw std::runtime_error("could not finish stem-export test WAV");
  }
}

TEST(AudioStemExportTest, RejectsInvalidRequest) {
  AudioStemExportRequest request;
  const auto result = export_production_audio_stems(request);
  EXPECT_FALSE(result);
}

TEST(AudioStemExportTest, ReportsTrimmedHandlesAndUsesAvailableSource) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("ve_stem_export_" + edit::EntityId::generate().toString());
  std::filesystem::create_directories(directory);
  const auto wav_path = directory / "source.wav";
  constexpr std::int64_t kTotal = 48'000;
  constexpr std::int64_t kHandle = 12'000;
  std::vector<std::int16_t> samples(static_cast<std::size_t>(kTotal), 0);
  for (std::int64_t index = 0; index < kHandle; ++index) {
    samples[static_cast<std::size_t>(index)] = 16'000;
  }
  for (std::int64_t index = kHandle; index < kTotal - kHandle; ++index) {
    samples[static_cast<std::size_t>(index)] = 4'000;
  }
  for (std::int64_t index = kTotal - kHandle; index < kTotal; ++index) {
    samples[static_cast<std::size_t>(index)] = 8'000;
  }
  write_stereo_wav(wav_path, samples);

  edit::Project project;
  edit::Asset asset;
  asset.name = "source.wav";
  asset.source_uri = wav_path.string();
  asset.duration = edit::Time(kTotal, audio_render::kTimelineAudioSampleRate);
  asset.has_audio = true;
  asset.audio_sample_rate = audio_render::kTimelineAudioSampleRate;
  asset.audio_channels = 2;
  project.assets.push_back(asset);

  edit::Clip clip;
  clip.asset_id = asset.id;
  clip.kind = edit::ClipKind::Audio;
  clip.name = "dialogue";
  clip.timeline_range =
      edit::TimeRange{edit::Time(0, audio_render::kTimelineAudioSampleRate),
                      edit::Time(kTotal - 2 * kHandle, audio_render::kTimelineAudioSampleRate)};
  clip.source_range =
      edit::TimeRange{edit::Time(kHandle, audio_render::kTimelineAudioSampleRate),
                      edit::Time(kTotal - 2 * kHandle, audio_render::kTimelineAudioSampleRate)};

  edit::Track track;
  track.kind = edit::TrackKind::Audio;
  track.clips.push_back(clip);
  edit::Sequence sequence;
  sequence.audio_sample_rate = audio_render::kTimelineAudioSampleRate;
  sequence.tracks.push_back(track);
  project.sequences.push_back(sequence);

  edit::TimelineEditor editor(std::move(project));
  auto snapshot = editor.snapshot(editor.projectAt(editor.revision())->sequences.front().id,
                                  editor.revision());
  ASSERT_TRUE(snapshot);

  auto registry = std::make_shared<audio_render::OriginalAudioRegistry>();
  ASSERT_TRUE(registry->register_original(asset.id, {wav_path, -1}));
  auto renderer = std::make_shared<audio_render::TimelineAudioRenderer>(registry);

  const auto dest = directory / "stems";
  AudioStemExportRequest request;
  request.snapshot = snapshot.value();
  request.audio_renderer = renderer;
  request.clip_ids = {clip.id};
  request.handle_before = edit::Time(1, 1);
  request.handle_after = edit::Time(1, 1);
  request.destination_directory = dest;
  const auto result = export_production_audio_stems(request);
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result.value().stems.size(), 1U);
  EXPECT_EQ(result.value().stems.front().warnings.size(), 2U);
  EXPECT_EQ(result.value().stems.front().sample_count, static_cast<std::uint64_t>(kTotal));
  EXPECT_TRUE(std::filesystem::exists(result.value().stems.front().path));
  std::ifstream wav(result.value().stems.front().path, std::ios::binary);
  ASSERT_TRUE(wav);
  wav.seekg(44);
  float first_sample = 0.0F;
  wav.read(reinterpret_cast<char*>(&first_sample), static_cast<std::streamsize>(sizeof(float)));
  EXPECT_GT(first_sample, 0.25F);

  std::filesystem::remove_all(directory);
}

} // namespace
} // namespace video_editor::export_service
