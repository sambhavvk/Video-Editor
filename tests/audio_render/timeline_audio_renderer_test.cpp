// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/original_audio_registry.h"
#include "video_editor/audio_render/timeline_audio_renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace video_editor::audio_render {
namespace {

constexpr std::uint32_t kFixtureFrames = 12'000;

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

void write_pcm_wav(const std::filesystem::path& path,
                   const std::span<const std::array<std::int16_t, 2>> samples,
                   const std::uint32_t sample_rate = kTimelineAudioSampleRate) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("could not create audio-render test WAV");
  }
  const auto data_size = static_cast<std::uint32_t>(samples.size() * 4U);
  output.write("RIFF", 4);
  write_u32(output, 36U + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16U);
  write_u16(output, 1U);
  write_u16(output, 2U);
  write_u32(output, sample_rate);
  write_u32(output, sample_rate * 4U);
  write_u16(output, 4U);
  write_u16(output, 16U);
  output.write("data", 4);
  write_u32(output, data_size);
  for (const auto sample : samples) {
    write_u16(output, static_cast<std::uint16_t>(sample[0]));
    write_u16(output, static_cast<std::uint16_t>(sample[1]));
  }
  if (!output) {
    throw std::runtime_error("could not finish audio-render test WAV");
  }
}

[[nodiscard]] std::string shell_quote(const std::filesystem::path& path) {
  const std::string value = path.string();
#ifdef _WIN32
  std::string result{"\""};
  for (const char character : value) {
    result += character == '"' ? "\"\"" : std::string(1, character);
  }
  return result + '"';
#else
  std::string result{"'"};
  for (const char character : value) {
    result += character == '\'' ? "'\\''" : std::string(1, character);
  }
  return result + '\'';
#endif
}

class GeneratedAudio final {
public:
  GeneratedAudio() {
    directory_ = std::filesystem::temp_directory_path() /
                 ("video_editor_audio_render_" + edit::EntityId::generate().toString());
    std::filesystem::create_directories(directory_);

    std::vector<std::array<std::int16_t, 2>> ramp(kFixtureFrames);
    std::vector<std::array<std::int16_t, 2>> constant(kFixtureFrames);
    std::vector<std::array<std::int16_t, 2>> constant_44k(11'025U, {4'096, 4'096});
    for (std::size_t index = 0; index < ramp.size(); ++index) {
      const auto phase = static_cast<std::int16_t>(index % 1'000U);
      ramp[index] = {static_cast<std::int16_t>(1'000 + phase),
                     static_cast<std::int16_t>(2'000 + phase)};
      constant[index] = {4'096, 4'096};
    }
    ramp_path_ = directory_ / "ramp.wav";
    constant_path_ = directory_ / "constant.wav";
    constant_44k_path_ = directory_ / "constant-44k.wav";
    write_pcm_wav(ramp_path_, ramp);
    write_pcm_wav(constant_path_, constant);
    write_pcm_wav(constant_44k_path_, constant_44k, 44'100U);

    nonzero_pts_path_ = directory_ / "nonzero-pts.mka";
    const std::string command =
        shell_quote(VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -y -itsoffset 2.5 -i " + shell_quote(ramp_path_) +
        " -map 0:a:0 -c:a pcm_s16le -output_ts_offset 2.5 " + shell_quote(nonzero_pts_path_);
    if (std::system(command.c_str()) != 0 || !std::filesystem::is_regular_file(nonzero_pts_path_)) {
      throw std::runtime_error("ffmpeg could not create the nonzero-PTS audio fixture");
    }
  }

  ~GeneratedAudio() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  GeneratedAudio(const GeneratedAudio&) = delete;
  GeneratedAudio& operator=(const GeneratedAudio&) = delete;

  [[nodiscard]] const std::filesystem::path& ramp() const noexcept {
    return ramp_path_;
  }
  [[nodiscard]] const std::filesystem::path& constant() const noexcept {
    return constant_path_;
  }
  [[nodiscard]] const std::filesystem::path& constant_44k() const noexcept {
    return constant_44k_path_;
  }
  [[nodiscard]] const std::filesystem::path& nonzero_pts() const noexcept {
    return nonzero_pts_path_;
  }

private:
  std::filesystem::path directory_;
  std::filesystem::path ramp_path_;
  std::filesystem::path constant_path_;
  std::filesystem::path constant_44k_path_;
  std::filesystem::path nonzero_pts_path_;
};

[[nodiscard]] const GeneratedAudio& fixture() {
  static const GeneratedAudio result;
  return result;
}

[[nodiscard]] float ramp_left(const std::int64_t source_sample) {
  return static_cast<float>(1'000 + (source_sample % 1'000)) / 32'768.0F;
}

struct TimelineFixture final {
  edit::Project project;
  edit::EntityId sequence_id;
  edit::EntityId ramp_asset_id;
  edit::EntityId constant_asset_id;
};

[[nodiscard]] TimelineFixture make_timeline() {
  TimelineFixture result;
  result.project.name = "Audio renderer test";

  edit::Asset ramp;
  ramp.name = "ramp.wav";
  ramp.duration = edit::Time(kFixtureFrames, kTimelineAudioSampleRate);
  ramp.has_audio = true;
  ramp.audio_sample_rate = kTimelineAudioSampleRate;
  ramp.audio_channels = 2;
  result.ramp_asset_id = ramp.id;
  result.project.assets.push_back(ramp);

  edit::Asset constant = ramp;
  constant.id = edit::EntityId::generate();
  constant.name = "constant.wav";
  result.constant_asset_id = constant.id;
  result.project.assets.push_back(constant);

  edit::Sequence sequence;
  sequence.name = "Main";
  sequence.audio_sample_rate = kTimelineAudioSampleRate;
  result.sequence_id = sequence.id;
  result.project.sequences.push_back(sequence);
  return result;
}

[[nodiscard]] edit::Clip audio_clip(const edit::EntityId asset_id,
                                    const std::int64_t timeline_start,
                                    const std::int64_t source_start, const std::int64_t duration) {
  edit::Clip clip;
  clip.asset_id = asset_id;
  clip.kind = edit::ClipKind::Audio;
  clip.name = "Audio";
  clip.timeline_range = edit::TimeRange(edit::Time(timeline_start, kTimelineAudioSampleRate),
                                        edit::Time(duration, kTimelineAudioSampleRate));
  clip.source_range = edit::TimeRange(edit::Time(source_start, kTimelineAudioSampleRate),
                                      edit::Time(duration, kTimelineAudioSampleRate));
  clip.audio_pan = -1.0;
  return clip;
}

[[nodiscard]] edit::Track audio_track(std::vector<edit::Clip> clips, const bool muted = false,
                                      const bool solo = false) {
  edit::Track track;
  track.kind = edit::TrackKind::Audio;
  track.name = "Audio";
  track.muted = muted;
  track.solo = solo;
  track.clips = std::move(clips);
  return track;
}

[[nodiscard]] edit::TimelineSnapshot snapshot(TimelineFixture fixture,
                                              std::vector<edit::Track> tracks) {
  fixture.project.sequences.front().tracks = std::move(tracks);
  edit::TimelineEditor editor(std::move(fixture.project));
  auto result = editor.snapshot(fixture.sequence_id, edit::Revision{0});
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return result.value();
}

[[nodiscard]] std::shared_ptr<OriginalAudioRegistry> registry_for(const TimelineFixture& timeline) {
  auto registry = std::make_shared<OriginalAudioRegistry>();
  if (!registry->register_original(timeline.ramp_asset_id, {fixture().ramp(), -1}) ||
      !registry->register_original(timeline.constant_asset_id, {fixture().constant(), -1})) {
    throw std::runtime_error("could not register audio test fixtures");
  }
  return registry;
}

void expect_silence(const audio::AudioBlock& block, const std::size_t index) {
  EXPECT_FLOAT_EQ(block.channel(0)[index], 0.0F);
  EXPECT_FLOAT_EQ(block.channel(1)[index], 0.0F);
}

TEST(OriginalAudioRegistry, StoresOnlyValidatedAuthoritativeOriginals) {
  OriginalAudioRegistry registry;
  const edit::EntityId asset_id = edit::EntityId::generate();
  EXPECT_FALSE(registry.register_original({}, {fixture().ramp(), -1}));
  EXPECT_FALSE(registry.register_original(asset_id, {{}, -1}));
  EXPECT_FALSE(registry.register_original(asset_id, {fixture().ramp(), -2}));
  EXPECT_TRUE(registry.register_original(asset_id, {fixture().ramp(), -1}));
  EXPECT_EQ(registry.size(), 1U);
  ASSERT_TRUE(registry.resolve_original(asset_id).has_value());
  EXPECT_TRUE(registry.unregister_asset(asset_id));
  EXPECT_FALSE(registry.resolve_original(asset_id).has_value());
}

TEST(TimelineAudioRenderer, PreservesExactRequestedRangeAndFillsTimelineGaps) {
  auto timeline = make_timeline();
  auto clip = audio_clip(timeline.ramp_asset_id, 100, 50, 100);
  const auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);
  const auto result = renderer.render(snapshot(timeline, {audio_track({clip})}),
                                      {.start_sample = 80, .sample_count = 150});
  ASSERT_TRUE(result) << result.error().message;
  const auto& block = result.value();
  EXPECT_EQ(block.start_sample(), 80);
  EXPECT_EQ(block.frame_count(), 150U);
  EXPECT_EQ(block.format().sample_rate, kTimelineAudioSampleRate);
  EXPECT_EQ(block.format().channels, kTimelineAudioChannels);
  expect_silence(block, 19U);
  EXPECT_NEAR(block.channel(0)[20], ramp_left(50), 0.00001F);
  EXPECT_NEAR(block.channel(0)[119], ramp_left(149), 0.00001F);
  expect_silence(block, 120U);
}

TEST(TimelineAudioRenderer, HonorsPlaybackRateAndReverseSourceMapping) {
  auto timeline = make_timeline();
  auto fast = audio_clip(timeline.ramp_asset_id, 0, 0, 20);
  fast.timeline_range.duration = edit::Time(10, kTimelineAudioSampleRate);
  fast.playback_rate = edit::Rate(2, 1);
  auto reversed = audio_clip(timeline.ramp_asset_id, 20, 10, 5);
  reversed.reversed = true;
  const auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);
  const auto result = renderer.render(snapshot(timeline, {audio_track({fast, reversed})}),
                                      {.start_sample = 0, .sample_count = 25});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_NEAR(result.value().channel(0)[0], ramp_left(0), 0.00001F);
  EXPECT_NEAR(result.value().channel(0)[1], ramp_left(2), 0.00001F);
  EXPECT_NEAR(result.value().channel(0)[9], ramp_left(18), 0.00001F);
  expect_silence(result.value(), 10U);
  EXPECT_NEAR(result.value().channel(0)[20], ramp_left(14), 0.00001F);
  EXPECT_NEAR(result.value().channel(0)[24], ramp_left(10), 0.00001F);
}

TEST(TimelineAudioRenderer, SumsTracksAndAppliesGainPanAndFades) {
  auto timeline = make_timeline();
  auto faded = audio_clip(timeline.constant_asset_id, 0, 0, 200);
  faded.audio_gain_db = -6.020599913279624;
  faded.fade_in = edit::Time(100, kTimelineAudioSampleRate);
  faded.fade_out = edit::Time(100, kTimelineAudioSampleRate);
  auto dry = audio_clip(timeline.constant_asset_id, 0, 0, 200);
  const auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);
  const auto result =
      renderer.render(snapshot(timeline, {audio_track({faded}), audio_track({dry})}),
                      {.start_sample = 0, .sample_count = 200});
  ASSERT_TRUE(result) << result.error().message;
  constexpr float dry_amplitude = 0.125F;
  EXPECT_NEAR(result.value().channel(0)[0], dry_amplitude, 0.00001F);
  EXPECT_NEAR(result.value().channel(0)[50], dry_amplitude + 0.03125F, 0.00002F);
  EXPECT_NEAR(result.value().channel(0)[100], dry_amplitude + 0.0625F, 0.00002F);
  EXPECT_NEAR(result.value().channel(0)[150], dry_amplitude + 0.03125F, 0.00002F);
  EXPECT_NEAR(result.value().channel(1)[100], 0.0F, 0.00001F);
}

TEST(TimelineAudioRenderer, MuteAndSoloResolveBeforeMixing) {
  auto timeline = make_timeline();
  auto ramp = audio_clip(timeline.ramp_asset_id, 0, 0, 20);
  auto constant = audio_clip(timeline.constant_asset_id, 0, 0, 20);
  const auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);

  const auto solo_result = renderer.render(
      snapshot(timeline, {audio_track({ramp}), audio_track({constant}, false, true)}),
      {.start_sample = 0, .sample_count = 20});
  ASSERT_TRUE(solo_result) << solo_result.error().message;
  EXPECT_NEAR(solo_result.value().channel(0)[0], 0.125F, 0.00001F);

  const auto muted_solo_result = renderer.render(
      snapshot(timeline, {audio_track({ramp}), audio_track({constant}, true, true)}),
      {.start_sample = 0, .sample_count = 20});
  ASSERT_TRUE(muted_solo_result) << muted_solo_result.error().message;
  expect_silence(muted_solo_result.value(), 0U);
}

TEST(TimelineAudioRenderer, NormalizesNonzeroStreamPtsToSourceRelativeTime) {
  auto timeline = make_timeline();
  auto registry = registry_for(timeline);
  ASSERT_TRUE(registry->register_original(timeline.ramp_asset_id, {fixture().nonzero_pts(), -1}));
  TimelineAudioRenderer renderer(registry);
  auto clip = audio_clip(timeline.ramp_asset_id, 0, 0, 32);
  const auto result = renderer.render(snapshot(timeline, {audio_track({clip})}),
                                      {.start_sample = 0, .sample_count = 32});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_NEAR(result.value().channel(0)[0], ramp_left(0), 0.00001F);
  EXPECT_NEAR(result.value().channel(0)[31], ramp_left(31), 0.00001F);
}

TEST(TimelineAudioRenderer, ResamplesAndFlushesTheRequestedSourceTail) {
  auto timeline = make_timeline();
  auto registry = registry_for(timeline);
  ASSERT_TRUE(
      registry->register_original(timeline.constant_asset_id, {fixture().constant_44k(), -1}));
  TimelineAudioRenderer renderer(registry);
  auto clip = audio_clip(timeline.constant_asset_id, 0, 0, 12'000);
  const auto result = renderer.render(snapshot(timeline, {audio_track({clip})}),
                                      {.start_sample = 11'968, .sample_count = 32});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_GT(result.value().channel(0)[0], 0.05F);
  EXPECT_GT(result.value().channel(0)[31], 0.05F);
}

TEST(TimelineAudioRenderer, ReturnsTypedErrorsForCancellationAndMissingMedia) {
  auto timeline = make_timeline();
  auto clip = audio_clip(timeline.ramp_asset_id, 0, 0, 100);
  auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);
  std::stop_source cancellation;
  ASSERT_TRUE(cancellation.request_stop());
  const auto cancelled = renderer.render(
      snapshot(timeline, {audio_track({clip})}),
      {.start_sample = 0, .sample_count = 100, .cancellation = cancellation.get_token()});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().code, AudioRenderErrorCode::Cancelled);

  ASSERT_TRUE(registry->unregister_asset(timeline.ramp_asset_id));
  const auto missing = renderer.render(snapshot(timeline, {audio_track({clip})}),
                                       {.start_sample = 0, .sample_count = 100});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, AudioRenderErrorCode::MissingMedia);
  EXPECT_EQ(missing.error().asset_id, timeline.ramp_asset_id);
  EXPECT_EQ(missing.error().clip_id, clip.id);
}

TEST(TimelineAudioRenderer, RepeatedRequestsAreBitForBitDeterministic) {
  auto timeline = make_timeline();
  auto clip = audio_clip(timeline.ramp_asset_id, 33, 77, 500);
  clip.audio_gain_db = -3.0;
  clip.audio_pan = 0.25;
  const auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);
  const auto frozen = snapshot(timeline, {audio_track({clip})});
  const AudioRenderRequest request{.start_sample = 25, .sample_count = 600};
  const auto first = renderer.render(frozen, request);
  const auto second = renderer.render(frozen, request);
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second) << second.error().message;
  EXPECT_TRUE(std::ranges::equal(first.value().channel(0), second.value().channel(0)));
  EXPECT_TRUE(std::ranges::equal(first.value().channel(1), second.value().channel(1)));
}

} // namespace
} // namespace video_editor::audio_render
