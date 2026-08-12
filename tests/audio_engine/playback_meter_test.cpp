// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/playback_meter.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>

namespace video_editor::audio {
namespace {

TEST(PlaybackMeter, ReportsZeroBeforeAnyProcessing) {
  PlaybackMeter meter;
  const PlaybackMeter::Reading reading = meter.read();
  EXPECT_FLOAT_EQ(reading.peak[0], 0.0F);
  EXPECT_FLOAT_EQ(reading.peak[1], 0.0F);
  EXPECT_FLOAT_EQ(reading.rms[0], 0.0F);
  EXPECT_FLOAT_EQ(reading.rms[1], 0.0F);
  EXPECT_EQ(reading.sample_count, 0U);
}

TEST(PlaybackMeter, TracksPeakAndRmsPerChannel) {
  PlaybackMeter meter;
  // 4 frames, 2 channels: left = [0.5, -0.5, 0.5, -0.5], right = [0.25, 0.25, 0.25, 0.25]
  const float samples[] = {0.5F, 0.25F, -0.5F, 0.25F, 0.5F, 0.25F, -0.5F, 0.25F};
  meter.process(samples, 4, 2);
  const PlaybackMeter::Reading reading = meter.read();
  EXPECT_FLOAT_EQ(reading.peak[0], 0.5F);
  EXPECT_FLOAT_EQ(reading.peak[1], 0.25F);
  // RMS of [0.5, -0.5, 0.5, -0.5] = sqrt((0.25*4)/4) = 0.5
  EXPECT_NEAR(reading.rms[0], 0.5F, 0.0001F);
  // RMS of [0.25, 0.25, 0.25, 0.25] = 0.25
  EXPECT_NEAR(reading.rms[1], 0.25F, 0.0001F);
  EXPECT_EQ(reading.sample_count, 4U);
}

TEST(PlaybackMeter, ReadResetsAccumulators) {
  PlaybackMeter meter;
  const float samples[] = {0.5F, 0.5F};
  meter.process(samples, 1, 2);
  (void)meter.read();
  const PlaybackMeter::Reading second = meter.read();
  EXPECT_FLOAT_EQ(second.peak[0], 0.0F);
  EXPECT_FLOAT_EQ(second.rms[0], 0.0F);
  EXPECT_EQ(second.sample_count, 0U);
}

TEST(PlaybackMeter, HandlesSilence) {
  PlaybackMeter meter;
  const float samples[] = {0.0F, 0.0F, 0.0F, 0.0F};
  meter.process(samples, 2, 2);
  const PlaybackMeter::Reading reading = meter.read();
  EXPECT_FLOAT_EQ(reading.peak[0], 0.0F);
  EXPECT_FLOAT_EQ(reading.peak[1], 0.0F);
  EXPECT_FLOAT_EQ(reading.rms[0], 0.0F);
  EXPECT_FLOAT_EQ(reading.rms[1], 0.0F);
  EXPECT_EQ(reading.sample_count, 2U);
}

TEST(PlaybackMeter, AccumulatesAcrossMultipleProcessCalls) {
  PlaybackMeter meter;
  const float first[] = {0.5F, 0.5F};
  const float second[] = {0.3F, 0.3F};
  meter.process(first, 1, 2);
  meter.process(second, 1, 2);
  const PlaybackMeter::Reading reading = meter.read();
  EXPECT_FLOAT_EQ(reading.peak[0], 0.5F);
  EXPECT_NEAR(reading.rms[0], std::sqrt((0.25F + 0.09F) / 2.0F), 0.0001F);
  EXPECT_EQ(reading.sample_count, 2U);
}

} // namespace
} // namespace video_editor::audio
