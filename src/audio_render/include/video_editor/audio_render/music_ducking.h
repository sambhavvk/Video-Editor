// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/time.h"

#include <cstddef>
#include <span>
#include <vector>

namespace video_editor::audio_render {

struct MusicDuckingOptions final {
  double threshold_dbfs{-24.0};
  double depth_db{-12.0};
  std::size_t attack_samples{5'760};   // 120 ms at 48 kHz
  std::size_t release_samples{19'200}; // 400 ms at 48 kHz
};

struct MusicDuckingKeyframe final {
  edit::Time time{};
  double gain_db{0.0};
};

// dialogue_peaks_dbfs contains one peak per fixed-size block in timeline order.
// Returns clip-local gain keyframes for an audio.volume effect on the music clip.
[[nodiscard]] std::vector<MusicDuckingKeyframe> generateMusicDuckingEnvelope(
    std::span<const float> dialogue_peaks_dbfs, std::size_t block_samples,
    edit::Time clip_duration, const MusicDuckingOptions& options = {});

} // namespace video_editor::audio_render
