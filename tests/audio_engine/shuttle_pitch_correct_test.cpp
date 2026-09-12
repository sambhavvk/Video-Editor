// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/realtime_playback.h"
#include "video_editor/audio_engine/shuttle_pitch_correct.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace video_editor::audio {
namespace {

[[nodiscard]] std::vector<float> sine_block(const std::size_t frame_count,
                                            const float frequency_hz,
                                            const float sample_rate) {
  std::vector<float> samples(frame_count);
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    const float phase = (2.0F * std::numbers::pi_v<float> * frequency_hz *
                         static_cast<float>(frame)) /
                        sample_rate;
    samples[frame] = std::sin(phase);
  }
  return samples;
}

[[nodiscard]] float estimate_pitch_hz(const std::span<const float> samples,
                                      const float sample_rate) {
  std::size_t best_lag = 0U;
  float best_correlation = -1.0F;
  const std::size_t min_lag =
      static_cast<std::size_t>(sample_rate / 1'200.0F);
  const std::size_t max_lag =
      static_cast<std::size_t>(sample_rate / 80.0F);
  for (std::size_t lag = min_lag; lag <= max_lag && lag < samples.size() / 2U; ++lag) {
    float correlation = 0.0F;
    for (std::size_t index = 0; index + lag < samples.size(); ++index) {
      correlation += samples[index] * samples[index + lag];
    }
    if (correlation > best_correlation) {
      best_correlation = correlation;
      best_lag = lag;
    }
  }
  if (best_lag == 0U) {
    return 0.0F;
  }
  return sample_rate / static_cast<float>(best_lag);
}

TEST(ShuttlePitchCorrectTest, SupportedForwardShuttleRates) {
  EXPECT_TRUE(shuttle_pitch_correction_supported(0.5));
  EXPECT_TRUE(shuttle_pitch_correction_supported(2.0));
  EXPECT_TRUE(shuttle_pitch_correction_supported(4.0));
  EXPECT_TRUE(shuttle_pitch_correction_supported(8.0));
  EXPECT_FALSE(shuttle_pitch_correction_supported(1.0));
  EXPECT_FALSE(shuttle_pitch_correction_supported(-2.0));
}

TEST(ShuttlePitchCorrectTest, DoubleSpeedPreservesPitch) {
  constexpr float kSampleRate = 48'000.0F;
  constexpr float kFrequencyHz = 440.0F;
  constexpr std::size_t kInputFrames = 9'216U;
  constexpr std::size_t kOutputFrames = kInputFrames / 2U;

  AudioBlock input(kPlaybackAudioFormat, 0, kInputFrames);
  AudioBlock output(kPlaybackAudioFormat, 0, kOutputFrames);
  const std::vector<float> tone = sine_block(kInputFrames, kFrequencyHz, kSampleRate);
  for (std::uint32_t channel = 0; channel < kPlaybackAudioFormat.channels; ++channel) {
    std::copy(tone.begin(), tone.end(), input.channel(channel).begin());
  }

  ShuttlePitchCorrector corrector;
  corrector.reset(2.0, kPlaybackAudioFormat.channels);
  corrector.stretch(input, output);

  const float estimated =
      estimate_pitch_hz(output.channel(0), kPlaybackAudioFormat.sample_rate);
  EXPECT_NEAR(estimated, kFrequencyHz, 35.0F);
}

TEST(ShuttlePitchCorrectTest, UnityRateIsUnsupported) {
  ShuttlePitchCorrector corrector;
  corrector.reset(1.0, kPlaybackAudioFormat.channels);
  AudioBlock input(kPlaybackAudioFormat, 0, 512U);
  AudioBlock output(kPlaybackAudioFormat, 0, 512U);
  EXPECT_THROW(corrector.stretch(input, output), std::logic_error);
}

} // namespace
} // namespace video_editor::audio
