// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/music_ducking.h"

#include <cmath>

namespace video_editor::audio_render {

std::vector<MusicDuckingKeyframe> generateMusicDuckingEnvelope(
    const std::span<const float> dialogue_peaks_dbfs, const std::size_t block_samples,
    const edit::Time clip_duration, const MusicDuckingOptions& options) {
  std::vector<MusicDuckingKeyframe> keyframes;
  if (dialogue_peaks_dbfs.empty() || block_samples == 0 || clip_duration.isZero() ||
      clip_duration.isNegative()) {
    return keyframes;
  }

  const std::size_t attack_blocks =
      std::max<std::size_t>(1, options.attack_samples / block_samples);
  const std::size_t release_blocks =
      std::max<std::size_t>(1, options.release_samples / block_samples);
  const auto block_time = edit::Time(static_cast<std::int64_t>(block_samples), 48'000)
                              .rescaledTo(clip_duration.timescale(),
                                          edit::RoundingMode::NearestTiesEven);

  double envelope_db = 0.0;
  keyframes.push_back({.time = edit::Time(0, clip_duration.timescale()), .gain_db = 0.0});
  for (std::size_t index = 0; index < dialogue_peaks_dbfs.size(); ++index) {
    const double target_db =
        dialogue_peaks_dbfs[index] >= options.threshold_dbfs ? options.depth_db : 0.0;
    const std::size_t smoothing = target_db < envelope_db ? attack_blocks : release_blocks;
    const double step = (target_db - envelope_db) / static_cast<double>(smoothing);
    envelope_db += step;
    const edit::Time time = block_time.scaled(static_cast<std::int64_t>(index + 1), 1,
                                              edit::RoundingMode::NearestTiesEven);
    if (time > clip_duration) {
      break;
    }
    if (std::abs(keyframes.back().gain_db - envelope_db) > 0.05) {
      keyframes.push_back({.time = time, .gain_db = envelope_db});
    }
  }
  if (keyframes.back().time != clip_duration) {
    keyframes.push_back({.time = clip_duration, .gain_db = envelope_db});
  }
  return keyframes;
}

} // namespace video_editor::audio_render
