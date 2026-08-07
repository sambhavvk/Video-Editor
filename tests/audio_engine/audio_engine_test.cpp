// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_engine/dsp.h"
#include "video_editor/audio_engine/loudness_meter.h"
#include "video_editor/audio_engine/spsc_audio_ring.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace video_editor::audio {
namespace {

TEST(AudioBlock, UsesPlanarStorageAndExactStartSample) {
  AudioBlock block({.sample_rate = 48'000, .channels = 2}, 960, 3);
  block.channel(0)[0] = 1.0F;
  block.channel(1)[0] = -1.0F;
  const auto interleaved = block.interleaved();
  EXPECT_EQ(block.start_sample(), 960);
  ASSERT_EQ(interleaved.size(), 6U);
  EXPECT_FLOAT_EQ(interleaved[0], 1.0F);
  EXPECT_FLOAT_EQ(interleaved[1], -1.0F);
}

TEST(SpscAudioRing, PreservesOrderAcrossWrapWithoutAllocation) {
  SpscAudioRing ring(3, 2);
  const std::array<float, 6> first{1, 2, 3, 4, 5, 6};
  EXPECT_EQ(ring.write(first), 3U);
  std::array<float, 4> partial{};
  EXPECT_EQ(ring.read(partial), 2U);
  EXPECT_EQ(partial, (std::array<float, 4>{1, 2, 3, 4}));

  const std::array<float, 4> second{7, 8, 9, 10};
  EXPECT_EQ(ring.write(second), 2U);
  std::array<float, 6> remainder{};
  EXPECT_EQ(ring.read(remainder), 3U);
  EXPECT_EQ(remainder, (std::array<float, 6>{5, 6, 7, 8, 9, 10}));
}

TEST(Dsp, AppliesGainPanAndLimiterDeterministically) {
  AudioBlock block({.sample_rate = 48'000, .channels = 2}, 0, 1);
  block.channel(0)[0] = 2.0F;
  block.channel(1)[0] = 2.0F;
  apply_gain(block, 0.5F);
  apply_stereo_pan(block, -1.0F);
  LookaheadFreeLimiter limiter(-6.0206F);
  limiter.process(block);
  EXPECT_NEAR(block.channel(0)[0], 0.5F, 0.001F);
  EXPECT_NEAR(block.channel(1)[0], 0.0F, 0.001F);
}

TEST(LevelMeter, MeasuresPeakAndRmsPerChannel) {
  AudioBlock block({.sample_rate = 48'000, .channels = 2}, 0, 4);
  block.channel(0)[0] = 1.0F;
  block.channel(1)[0] = -0.5F;
  const LevelReading levels = measure_levels(block);
  EXPECT_FLOAT_EQ(levels.peak[0], 1.0F);
  EXPECT_FLOAT_EQ(levels.peak[1], 0.5F);
  EXPECT_NEAR(levels.rms[0], 0.5F, 0.0001F);
  EXPECT_NEAR(levels.rms[1], 0.25F, 0.0001F);
}

TEST(LoudnessMeter, AcceptsMatchingBlocksAndReportsSamplePeak) {
  AudioBlock block({.sample_rate = 48'000, .channels = 2}, 0, 48'000);
  for (std::size_t frame = 0; frame < block.frame_count(); ++frame) {
    const float sample = 0.25F * std::sin(static_cast<float>(frame) * 0.03F);
    block.channel(0)[frame] = sample;
    block.channel(1)[frame] = sample;
  }
  LoudnessMeter meter(block.format());
  ASSERT_TRUE(meter.valid());
  ASSERT_TRUE(meter.add(block));
  const LoudnessReading reading = meter.reading();
  ASSERT_EQ(reading.sample_peak_dbfs.size(), 2U);
  EXPECT_NEAR(reading.sample_peak_dbfs[0], -12.04, 0.1);
}

} // namespace
} // namespace video_editor::audio

