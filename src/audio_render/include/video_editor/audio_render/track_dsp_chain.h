// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_engine/dsp.h"
#include "video_editor/edit_model/model.h"

#include <memory>
#include <string>
#include <vector>

namespace video_editor::audio_render {

// Parameter names for the audio effects. These are the canonical strings used
// in `Effect::parameters` and persisted in project snapshots.
inline constexpr std::string_view kEqFrequencyHz = "frequency_hz";
inline constexpr std::string_view kEqQuality = "quality";
inline constexpr std::string_view kEqGainDb = "gain_db";

inline constexpr std::string_view kCompressorThresholdDb = "threshold_db";
inline constexpr std::string_view kCompressorRatio = "ratio";
inline constexpr std::string_view kCompressorAttackMs = "attack_ms";
inline constexpr std::string_view kCompressorReleaseMs = "release_ms";
inline constexpr std::string_view kCompressorMakeupDb = "makeup_db";

inline constexpr std::string_view kLimiterCeilingDb = "ceiling_db";

inline constexpr std::string_view kDenoiseStrength = "strength";
inline constexpr std::string_view kDenoiseThresholdDb = "threshold_db";

// A stateful DSP chain that applies track-level audio effects to a mixed
// AudioBlock. The chain is constructed from a track's `effects` vector and
// preserves filter state across blocks so it can process a continuous stream.
// Processing runs on the pre-render worker thread, never in the audio device
// callback. Unknown or disabled effects are skipped. Effect order follows the
// vector order: typically EQ → compressor → denoise → limiter.
class TrackDspChain final {
public:
  TrackDspChain();
  ~TrackDspChain();
  TrackDspChain(const TrackDspChain&) = delete;
  TrackDspChain& operator=(const TrackDspChain&) = delete;
  TrackDspChain(TrackDspChain&&) noexcept;
  TrackDspChain& operator=(TrackDspChain&&) noexcept;

  // Build a chain from a track's effects vector. Unknown effect types are
  // ignored (the `Effect::known` flag is advisory). Returns an empty chain if
  // the vector is empty.
  void configure(const std::vector<edit::Effect>& effects, float sample_rate);

  // Process a mixed block in place. No-op if the chain is empty.
  void process(audio::AudioBlock& block) noexcept;

  [[nodiscard]] bool empty() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::audio_render
