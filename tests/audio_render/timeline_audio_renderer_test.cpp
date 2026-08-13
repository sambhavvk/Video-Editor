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

[[nodiscard]] edit::Effect audio_effect(const std::string& type) {
  edit::Effect effect;
  effect.type = type;
  effect.enabled = true;
  return effect;
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

TEST(TimelineAudioRenderer, TrackGainAndPanApplyAsMixerStageOverClipMix) {
  auto timeline = make_timeline();
  auto clip = audio_clip(timeline.constant_asset_id, 0, 0, 32);
  const auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);

  // Baseline: default track gain (0 dB) and pan (0) is unity.
  edit::Track default_track = audio_track({clip});
  const auto baseline =
      renderer.render(snapshot(timeline, {default_track}), {.start_sample = 0, .sample_count = 32});
  ASSERT_TRUE(baseline) << baseline.error().message;

  // +6 dB track gain doubles amplitude; pan stays center.
  edit::Track boosted = audio_track({clip});
  boosted.audio_gain_db = 6.020599913279624;
  const auto boosted_result =
      renderer.render(snapshot(timeline, {boosted}), {.start_sample = 0, .sample_count = 32});
  ASSERT_TRUE(boosted_result) << boosted_result.error().message;
  for (std::size_t i = 0; i < 32; ++i) {
    EXPECT_NEAR(boosted_result.value().channel(0)[i], baseline.value().channel(0)[i] * 2.0F,
                0.0001F);
  }
}

TEST(TimelineAudioRenderer, PublishesStablePostDspTrackMetersWithMuteSoloAndReorder) {
  auto timeline = make_timeline();
  auto audible_clip = audio_clip(timeline.constant_asset_id, 0, 0, 32);
  audible_clip.audio_pan = 0.0;
  auto first = audio_track({audible_clip});
  first.name = "First";
  auto muted = audio_track({audio_clip(timeline.ramp_asset_id, 0, 0, 32)}, true, false);
  muted.name = "Muted";
  auto solo = audio_track({audio_clip(timeline.constant_asset_id, 0, 0, 32)}, false, true);
  solo.name = "Solo";
  auto limiter = audio_effect("audio.limiter");
  limiter.parameters["ceiling_db"] = {.id = "ceiling_db", .value = -20.0, .keyframes = {}};
  solo.effects.push_back(std::move(limiter));
  const auto first_id = first.id;
  const auto muted_id = muted.id;
  const auto solo_id = solo.id;
  const auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);
  const auto initial = renderer.render(snapshot(timeline, {first, muted, solo}),
                                       {.start_sample = 0, .sample_count = 32});
  ASSERT_TRUE(initial) << initial.error().message;
  const auto meters = renderer.trackMeters();
  ASSERT_EQ(meters.tracks.size(), 3U);
  EXPECT_FALSE(meters.stale);
  EXPECT_EQ(meters.revision, edit::Revision{0});
  const auto find_meter = [&meters](const edit::EntityId id) -> const TrackMeterReading& {
    const auto found =
        std::find_if(meters.tracks.begin(), meters.tracks.end(),
                     [id](const TrackMeterReading& meter) { return meter.track_id == id; });
    EXPECT_NE(found, meters.tracks.end());
    return *found;
  };
  EXPECT_FALSE(find_meter(first_id).active);
  EXPECT_FALSE(find_meter(muted_id).active);
  EXPECT_TRUE(find_meter(solo_id).active);
  // The meter is sampled after the track limiter, not from the pre-DSP mix.
  EXPECT_GT(find_meter(solo_id).peak[0], 0.0F);
  EXPECT_LT(find_meter(solo_id).peak[0], 0.11F);

  // Reordering does not change ID association. Removing a track removes only
  // its reading on the next bounded snapshot; no index mapping is retained.
  const auto reordered =
      renderer.render(snapshot(timeline, {solo, first}), {.start_sample = 0, .sample_count = 32});
  ASSERT_TRUE(reordered) << reordered.error().message;
  const auto reordered_meters = renderer.trackMeters();
  EXPECT_EQ(reordered_meters.tracks.size(), 2U);
  EXPECT_EQ(reordered_meters.tracks[0].track_id, solo_id);
  EXPECT_EQ(reordered_meters.tracks[1].track_id, first_id);
  EXPECT_EQ(std::find_if(
                reordered_meters.tracks.begin(), reordered_meters.tracks.end(),
                [muted_id](const TrackMeterReading& meter) { return meter.track_id == muted_id; }),
            reordered_meters.tracks.end());
}

TEST(TimelineAudioRenderer, MutedAndNonSoloTracksDoNotDecodeOrFail) {
  auto timeline = make_timeline();
  auto missing = audio_track({audio_clip(timeline.ramp_asset_id, 0, 0, 32)}, true, false);
  auto solo = audio_track({audio_clip(timeline.constant_asset_id, 0, 0, 32)}, false, true);
  const auto registry = std::make_shared<OriginalAudioRegistry>();
  ASSERT_TRUE(registry->register_original(timeline.constant_asset_id, {fixture().constant(), -1}));
  TimelineAudioRenderer renderer(registry);
  const auto result =
      renderer.render(snapshot(timeline, {missing, solo}), {.start_sample = 0, .sample_count = 32});
  ASSERT_TRUE(result) << result.error().message;
  const auto meters = renderer.trackMeters();
  ASSERT_EQ(meters.tracks.size(), 2U);
  EXPECT_FALSE(meters.tracks[0].active);
  EXPECT_TRUE(meters.tracks[1].active);
}

TEST(TimelineAudioRenderer, SelectsTrackMetersAtAudioMasterPosition) {
  auto timeline = make_timeline();
  const auto loud_clip = audio_clip(timeline.constant_asset_id, 0, 0, 32);
  const auto quiet_clip = audio_clip(timeline.ramp_asset_id, 32, 0, 32);
  auto track = audio_track({loud_clip, quiet_clip});
  const auto track_id = track.id;
  TimelineAudioRenderer renderer(registry_for(timeline));
  const auto frozen = snapshot(timeline, {track});

  ASSERT_TRUE(renderer.render(frozen, {.start_sample = 0, .sample_count = 32}));
  ASSERT_TRUE(renderer.render(frozen, {.start_sample = 32, .sample_count = 32}));

  const auto first = renderer.trackMetersAt(16);
  const auto second = renderer.trackMetersAt(48);
  ASSERT_EQ(first.tracks.size(), 1U);
  ASSERT_EQ(second.tracks.size(), 1U);
  EXPECT_EQ(first.tracks.front().track_id, track_id);
  EXPECT_EQ(second.tracks.front().track_id, track_id);
  EXPECT_EQ(first.start_sample, 0);
  EXPECT_EQ(first.end_sample, 32);
  EXPECT_EQ(second.start_sample, 32);
  EXPECT_EQ(second.end_sample, 64);
  EXPECT_FALSE(first.stale);
  EXPECT_FALSE(second.stale);
  EXPECT_GT(first.tracks.front().peak[0], second.tracks.front().peak[0]);

  const auto outside = renderer.trackMetersAt(96);
  EXPECT_TRUE(outside.stale);
  EXPECT_EQ(outside.tracks.front().track_id, track_id);
}

TEST(TimelineAudioRenderer, TrackPanAtCenterIsUnityAndFullLeftRoutesToLeftOnly) {
  auto timeline = make_timeline();
  auto clip = audio_clip(timeline.constant_asset_id, 0, 0, 16);
  // Use a centered clip (pan 0) so the clip stage outputs both channels.
  clip.audio_pan = 0.0;
  const auto registry = registry_for(timeline);
  TimelineAudioRenderer renderer(registry);

  edit::Track center = audio_track({clip});
  center.audio_pan = 0.0;
  const auto center_result =
      renderer.render(snapshot(timeline, {center}), {.start_sample = 0, .sample_count = 16});
  ASSERT_TRUE(center_result) << center_result.error().message;

  edit::Track full_left = audio_track({clip});
  full_left.audio_pan = -1.0;
  const auto left_result =
      renderer.render(snapshot(timeline, {full_left}), {.start_sample = 0, .sample_count = 16});
  ASSERT_TRUE(left_result) << left_result.error().message;
  for (std::size_t i = 0; i < 16; ++i) {
    // Center track pan 0 must be unity (no attenuation).
    EXPECT_NEAR(center_result.value().channel(0)[i], center_result.value().channel(1)[i], 0.00001F);
    // Full-left track pan routes all energy to the left channel.
    EXPECT_NEAR(left_result.value().channel(0)[i], center_result.value().channel(0)[i], 0.0001F);
    EXPECT_NEAR(left_result.value().channel(1)[i], 0.0F, 0.0001F);
  }
}

TEST(TimelineAudioRenderer, StatefulTrackDspIsInvariantToContiguousRequestPartitioning) {
  auto timeline = make_timeline();
  auto clip = audio_clip(timeline.constant_asset_id, 0, 0, 2'400);
  edit::Track track = audio_track({clip});
  auto eq = audio_effect("audio.eq");
  eq.parameters["frequency_hz"] = {.id = "frequency_hz", .value = 1'000.0};
  eq.parameters["quality"] = {.id = "quality", .value = 0.707};
  eq.parameters["gain_db"] = {.id = "gain_db", .value = 3.0};
  auto compressor = audio_effect("audio.compressor");
  compressor.parameters["threshold_db"] = {.id = "threshold_db", .value = -18.0};
  compressor.parameters["ratio"] = {.id = "ratio", .value = 4.0};
  auto denoise = audio_effect("audio.dialogue_denoise");
  denoise.parameters["strength"] = {.id = "strength", .value = 0.2};
  denoise.parameters["threshold_db"] = {.id = "threshold_db", .value = -40.0};
  auto limiter = audio_effect("audio.limiter");
  limiter.parameters["ceiling_db"] = {.id = "ceiling_db", .value = -1.0};
  // Deliberately persist them in a non-canonical order; the renderer owns the
  // canonical EQ -> compressor -> denoise -> limiter order.
  track.effects = {limiter, denoise, compressor, eq};
  const auto frozen = snapshot(timeline, {track});
  const auto registry = registry_for(timeline);

  TimelineAudioRenderer whole_renderer(registry);
  const auto whole = whole_renderer.render(frozen, {.start_sample = 0, .sample_count = 2'400});
  ASSERT_TRUE(whole) << whole.error().message;

  TimelineAudioRenderer partitioned_renderer(registry);
  const auto first =
      partitioned_renderer.render(frozen, {.start_sample = 0, .sample_count = 1'200});
  const auto second =
      partitioned_renderer.render(frozen, {.start_sample = 1'200, .sample_count = 1'200});
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second) << second.error().message;
  for (std::size_t index = 0; index < 1'200; ++index) {
    EXPECT_FLOAT_EQ(first.value().channel(0)[index], whole.value().channel(0)[index]);
    EXPECT_FLOAT_EQ(first.value().channel(1)[index], whole.value().channel(1)[index]);
    EXPECT_FLOAT_EQ(second.value().channel(0)[index], whole.value().channel(0)[index + 1'200]);
    EXPECT_FLOAT_EQ(second.value().channel(1)[index], whole.value().channel(1)[index + 1'200]);
  }
}

} // namespace
} // namespace video_editor::audio_render
