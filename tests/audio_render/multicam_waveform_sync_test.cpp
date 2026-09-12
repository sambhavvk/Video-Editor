// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/multicam_waveform_sync.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace video_editor::audio_render {
namespace {

TEST(MulticamWaveformSyncTest, RecoversKnownPositiveOffsetWithinTolerance) {
  std::vector<float> reference(4'000, 0.0F);
  for (std::size_t index = 0; index < reference.size(); ++index) {
    reference[index] = std::sin(static_cast<float>(index) * 0.01F);
  }
  const std::size_t lag = 512;
  std::vector<float> candidate(reference.size() + lag, 0.0F);
  for (std::size_t index = 0; index < reference.size(); ++index) {
    candidate[index + lag] = reference[index];
  }
  const auto match = matchWaveformOffset(reference, candidate, 48'000);
  ASSERT_TRUE(match.has_value());
  EXPECT_GE(match->confidence, 0.35);
  EXPECT_EQ(match->offset, edit::Time(static_cast<std::int64_t>(lag), 48'000));
  EXPECT_EQ(match->lag_samples, static_cast<std::int64_t>(lag));
}

TEST(MulticamWaveformSyncTest, RecoversNegativeLagOnEqualLengthClips) {
  std::vector<float> reference(4'000, 0.0F);
  for (std::size_t index = 0; index < reference.size(); ++index) {
    reference[index] = std::sin(static_cast<float>(index) * 0.01F);
  }
  const std::size_t lag = 512;
  std::vector<float> candidate(reference.size(), 0.0F);
  for (std::size_t index = 0; index + lag < reference.size(); ++index) {
    candidate[index] = reference[index + lag];
  }
  const auto match = matchWaveformOffset(reference, candidate, 48'000);
  ASSERT_TRUE(match.has_value());
  EXPECT_GE(match->confidence, 0.35);
  EXPECT_EQ(match->lag_samples, -static_cast<std::int64_t>(lag));
}

TEST(MulticamWaveformSyncTest, RecoversPositiveLagWhenLengthsMatch) {
  std::vector<float> reference(4'000, 0.0F);
  for (std::size_t index = 0; index < reference.size(); ++index) {
    reference[index] = std::sin(static_cast<float>(index) * 0.01F);
  }
  const std::size_t lag = 256;
  std::vector<float> candidate(reference.size(), 0.0F);
  for (std::size_t index = 0; index + lag < candidate.size(); ++index) {
    candidate[index + lag] = reference[index];
  }
  const auto match = matchWaveformOffset(reference, candidate, 48'000);
  ASSERT_TRUE(match.has_value());
  EXPECT_EQ(match->lag_samples, static_cast<std::int64_t>(lag));
}

} // namespace
} // namespace video_editor::audio_render
