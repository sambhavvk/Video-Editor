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
}

} // namespace
} // namespace video_editor::audio_render
