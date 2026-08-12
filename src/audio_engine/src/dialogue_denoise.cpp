// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/dialogue_denoise.h"

#include <algorithm>
#include <cmath>

namespace video_editor::audio {

namespace {

float db_to_linear(float db) { return std::pow(10.0F, db / 20.0F); }

} // namespace

DialogueDenoise::DialogueDenoise(const float sample_rate) : sample_rate_(sample_rate) {
  if (sample_rate <= 0.0F) {
    sample_rate_ = 48'000.0F;
  }
  configure(0.5F, -40.0F);
}

void DialogueDenoise::configure(const float strength, const float threshold_db) noexcept {
  strength_ = std::clamp(strength, 0.0F, 1.0F);
  threshold_db_ = threshold_db;
}

void DialogueDenoise::reset() noexcept {
  for (auto& channel : channels_) {
    channel = {};
  }
}

void DialogueDenoise::process(AudioBlock& block) noexcept {
  if (strength_ <= 0.0F) {
    return;
  }
  const std::uint32_t channels = block.format().channels;
  if (channels == 0U || channels > 2U) {
    return;
  }
  channel_count_ = channels;
  // Attack/release coefficients for the envelope follower. Attack is fast (5ms)
  // to catch transients; release is slow (300ms) to hold the envelope through
  // pauses in dialogue.
  const float attack = std::exp(-1.0F / (0.001F * 5.0F * sample_rate_));
  const float release = std::exp(-1.0F / (0.001F * 300.0F * sample_rate_));
  // Noise-floor estimator: rises toward the envelope when the envelope is
  // below the current floor (capturing the background level during quiet
  // passages), but holds steady when the envelope is above the floor (so
  // dialogue doesn't raise the gate threshold). A very slow decay lets the
  // floor eventually adapt if the background level drops.
  const float noise_rise = std::exp(-1.0F / (0.001F * 100.0F * sample_rate_));
  const float noise_decay = std::exp(-1.0F / (0.001F * 10000.0F * sample_rate_));
  const float threshold_linear = db_to_linear(threshold_db_);

  for (std::size_t c = 0; c < channel_count_; ++c) {
    ChannelState& state = channels_[c];
    auto samples = block.channel(c);
    for (float& sample : samples) {
      const float abs_sample = std::abs(sample);
      const float coefficient = abs_sample > state.envelope ? attack : release;
      state.envelope = (coefficient * state.envelope) + ((1.0F - coefficient) * abs_sample);
      // Noise floor rises toward envelope during quiet passages (envelope <
      // floor), holds during loud passages (envelope > floor), with a very
      // slow decay so it adapts to long-term changes.
      if (state.envelope < state.noise_floor) {
        state.noise_floor = (noise_rise * state.noise_floor) +
                            ((1.0F - noise_rise) * state.envelope);
      } else {
        state.noise_floor = (noise_decay * state.noise_floor) +
                            ((1.0F - noise_decay) * state.envelope);
      }
      // Gate when envelope is below noise_floor + threshold (in linear terms,
      // threshold is multiplicative above the noise floor).
      const float gate_threshold = state.noise_floor * threshold_linear;
      float gain = 1.0F;
      if (state.envelope < gate_threshold) {
        // Attenuate proportionally to how far below threshold we are.
        const float ratio = state.envelope / std::max(gate_threshold, 1.0e-12F);
        gain = 1.0F - (strength_ * (1.0F - ratio));
        gain = std::clamp(gain, 1.0F - strength_, 1.0F);
      }
      sample *= gain;
    }
  }
}

} // namespace video_editor::audio
