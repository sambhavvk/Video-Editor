// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/edit_model/timeline_editor.h"

#include <memory>
#include <optional>
#include <string>

namespace video_editor::audio_render {

struct LoudnessNormalizeError final {
  std::string message;
};

struct LoudnessNormalizeResult final {
  double integrated_lufs{0.0};
  double gain_db{0.0};  // add this to the track/clip gain to hit target
};

using LoudnessNormalizeOutcome = edit::Result<LoudnessNormalizeResult, LoudnessNormalizeError>;

// Renders the entire timeline in blocks, measures integrated LUFS, and returns
// the gain offset in dB to add to reach target_lufs. Returns an error if the
// timeline is empty or rendering fails. Default target is -23 LUFS (EBU R128
// broadcast standard). Block size is 960 frames (20ms at 48kHz) to match the
// realtime render block.
[[nodiscard]] LoudnessNormalizeOutcome compute_normalization_gain(
    const edit::TimelineSnapshot& snapshot,
    std::shared_ptr<const OriginalAudioProvider> originals,
    double target_lufs = -23.0);

} // namespace video_editor::audio_render
