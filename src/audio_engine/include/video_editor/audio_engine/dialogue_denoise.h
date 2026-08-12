// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"

#include <cstddef>

namespace video_editor::audio {

// A lightweight, single-channel spectral-gate dialogue noise reducer. It
// estimates a noise floor from quiet passages and attenuates samples that fall
// below a threshold relative to the running signal envelope. This is not a
// full spectral subtractor; it is a computationally cheap expander-style gate
// tuned for dialogue. State is preserved across blocks so it can process a
// continuous stream.
//
// The detector tracks a slow envelope (attack 5 ms, release 300 ms) and a
// slower noise-floor estimate. Samples whose envelope falls below
// `noise_floor + threshold_db` are attenuated by `strength` (0..1, where 1 is
// full gating and 0 is bypass). A short look-ahead is approximated by a
// one-block delay of the envelope.
class DialogueDenoise final {
public:
  explicit DialogueDenoise(float sample_rate = 48'000.0F);
  ~DialogueDenoise() = default;

  DialogueDenoise(const DialogueDenoise&) = delete;
  DialogueDenoise& operator=(const DialogueDenoise&) = delete;
  DialogueDenoise(DialogueDenoise&&) noexcept = default;
  DialogueDenoise& operator=(DialogueDenoise&&) noexcept = default;

  // `strength` is 0..1 (0 = bypass, 1 = full gating). `threshold_db` is the
  // dB above the estimated noise floor at which gating begins.
  void configure(float strength, float threshold_db) noexcept;
  void reset() noexcept;
  void process(AudioBlock& block) noexcept;

private:
  float sample_rate_;
  float strength_{0.5F};
  float threshold_db_{-40.0F};
  // Per-channel envelope followers and noise-floor estimates.
  struct ChannelState {
    float envelope{0.0F};
    float noise_floor{1.0e-6F};
  };
  ChannelState channels_[2];
  std::size_t channel_count_{2};
};

} // namespace video_editor::audio
