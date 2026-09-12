// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"

#include <cstdint>
#include <vector>

namespace video_editor::audio {

// Preview shuttle rates that use pitch-preserving time stretch instead of
// resampled sample picking. Reverse transport stays on the legacy path.
[[nodiscard]] bool shuttle_pitch_correction_supported(double transport_rate) noexcept;

// Streaming WSOLA time stretcher for J/K/L shuttle preview audio. A speed
// factor above 1 compresses time (faster playback); below 1 expands it.
class ShuttlePitchCorrector {
public:
  void reset(double speed_factor, std::uint32_t channels) noexcept;
  void stretch(const AudioBlock& source, AudioBlock& output);

private:
  double speed_factor_{1.0};
  std::uint32_t channels_{0};
  std::vector<std::vector<float>> overlap_;
  bool primed_{false};
};

} // namespace video_editor::audio
