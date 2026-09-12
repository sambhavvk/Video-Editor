// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/music_ducking.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace video_editor::audio_render {
namespace {

TEST(MusicDucking, AttenuatesDuringDialogueAndRecoversAfter) {
  const std::vector<float> peaks{-40.0F, -18.0F, -16.0F, -40.0F, -42.0F, -44.0F, -46.0F};
  MusicDuckingOptions options;
  options.threshold_dbfs = -24.0;
  options.depth_db = -12.0;
  options.attack_samples = 48;
  options.release_samples = 96;
  const auto keyframes =
      generateMusicDuckingEnvelope(peaks, 48, edit::Time(7, 1), options);
  ASSERT_FALSE(keyframes.empty());
  const bool has_duck = std::any_of(keyframes.begin(), keyframes.end(),
                                    [](const MusicDuckingKeyframe& keyframe) {
                                      return keyframe.gain_db < -1.0;
                                    });
  EXPECT_TRUE(has_duck);
  EXPECT_NEAR(keyframes.back().gain_db, 0.0, 1.0);
}

TEST(MusicDucking, ReleaseTakesLongerThanAttack) {
  std::vector<float> peaks(16, -50.0F);
  peaks[2] = -12.0F;
  peaks[3] = -12.0F;
  MusicDuckingOptions options;
  options.threshold_dbfs = -24.0;
  options.depth_db = -12.0;
  options.attack_samples = 48;
  options.release_samples = 480;
  const auto keyframes =
      generateMusicDuckingEnvelope(peaks, 48, edit::Time(16, 1), options);
  ASSERT_GE(keyframes.size(), 4U);
  std::size_t trough = 0;
  for (std::size_t index = 1; index < keyframes.size(); ++index) {
    if (keyframes[index].gain_db < keyframes[trough].gain_db) {
      trough = index;
    }
  }
  ASSERT_LT(keyframes[trough].gain_db, -8.0);
  ASSERT_LT(trough + 1, keyframes.size());
  EXPECT_LT(keyframes[trough + 1].gain_db, -1.0);
}

TEST(MusicDucking, NoDialogueKeepsUnityGain) {
  const std::vector<float> peaks{-50.0F, -48.0F, -46.0F};
  const auto keyframes = generateMusicDuckingEnvelope(peaks, 48, edit::Time(2, 1));
  for (const auto& keyframe : keyframes) {
    EXPECT_NEAR(keyframe.gain_db, 0.0, 0.01);
  }
}

} // namespace
} // namespace video_editor::audio_render
