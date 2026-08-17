// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/silence_detector.h"

#include <gtest/gtest.h>

#include <limits>

namespace {
namespace audio = video_editor::audio;
namespace render = video_editor::audio_render;

TEST(SilenceDetector, ReturnsExactAbsoluteHalfOpenRangesAndMerges) {
  audio::AudioBlock block({48'000, 2}, 100, 1'440);
  std::fill(block.channel(0).begin() + 480, block.channel(0).end(), 0.1F);
  std::fill(block.channel(1).begin() + 480, block.channel(1).end(), 0.1F);
  const auto result = render::detectSilence(
      block, {.analysis_window_samples = 480, .minimum_silence_samples = 480});
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 1U);
  EXPECT_EQ(result.value().front(), (render::SilenceRange{100, 580}));
}

TEST(SilenceDetector, RejectsNonCanonicalFormatAndOverflowingRange) {
  audio::AudioBlock wrong_rate({44'100, 2}, 0, 10);
  EXPECT_FALSE(render::detectSilence(wrong_rate));
  audio::AudioBlock block({48'000, 2}, std::numeric_limits<std::int64_t>::max() - 4, 10);
  const auto result = render::detectSilence(block);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, render::SilenceErrorCode::InvalidAudio);
}

TEST(SilenceDetector, RejectsNonFiniteSamples) {
  audio::AudioBlock block({48'000, 1}, 0, 480);
  block.channel(0)[12] = std::numeric_limits<float>::quiet_NaN();
  const auto result = render::detectSilence(block);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, render::SilenceErrorCode::InvalidAudio);
}

TEST(SilenceDetector, AccumulatorMergesSilenceAcrossBoundedRenderChunks) {
  render::SilenceAccumulator accumulator({.analysis_window_samples = 480,
                                          .minimum_silence_samples = 2'400,
                                          .merge_gap_samples = 2'400});
  audio::AudioBlock first({48'000, 2}, 10'000, 1'200);
  audio::AudioBlock second({48'000, 2}, 11'200, 1'200);
  audio::AudioBlock third({48'000, 2}, 12'400, 480);
  third.channel(0)[0] = 0.1F;
  third.channel(1)[0] = 0.1F;
  ASSERT_TRUE(accumulator.add(first));
  ASSERT_TRUE(accumulator.add(second));
  ASSERT_TRUE(accumulator.add(third));
  const auto result = accumulator.finish();
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 1U);
  EXPECT_EQ(result.value().front(), (render::SilenceRange{10'000, 12'400}));
}

TEST(SilenceDetector, AccumulatorRejectsChannelLayoutChangesAcrossChunks) {
  render::SilenceAccumulator accumulator;
  audio::AudioBlock mono(audio::AudioFormat{48'000, 1}, 0, 480);
  audio::AudioBlock stereo(audio::AudioFormat{48'000, 2}, 480, 480);

  ASSERT_TRUE(accumulator.add(mono));
  EXPECT_FALSE(accumulator.add(stereo));
  ASSERT_TRUE(accumulator.error().has_value());
  EXPECT_EQ(accumulator.error()->code, render::SilenceErrorCode::InvalidFormat);
}

} // namespace
