// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/dsp.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace video_editor::audio {
namespace {

float db_to_linear(const float decibels) { return std::pow(10.0F, decibels / 20.0F); }

float linear_to_db(const float value) {
  return 20.0F * std::log10(std::max(value, 1.0e-12F));
}

} // namespace

void apply_gain(AudioBlock& block, const float linear_gain) noexcept {
  for (std::size_t channel = 0; channel < block.format().channels; ++channel) {
    for (float& sample : block.channel(channel)) {
      sample *= linear_gain;
    }
  }
}

void apply_stereo_pan(AudioBlock& block, const float pan) noexcept {
  if (block.format().channels < 2U) {
    return;
  }
  const float limited_pan = std::clamp(pan, -1.0F, 1.0F);
  const float angle = (limited_pan + 1.0F) * (std::numbers::pi_v<float> / 4.0F);
  const float left_gain = std::cos(angle);
  const float right_gain = std::sin(angle);
  for (float& sample : block.channel(0)) {
    sample *= left_gain;
  }
  for (float& sample : block.channel(1)) {
    sample *= right_gain;
  }
}

Biquad::Biquad(const float b0, const float b1, const float b2, const float a1,
               const float a2) noexcept
    : b0_(b0), b1_(b1), b2_(b2), a1_(a1), a2_(a2) {}

Biquad Biquad::normalized(const float b0, const float b1, const float b2, const float a0,
                          const float a1, const float a2) {
  if (std::abs(a0) < 1.0e-12F) {
    throw std::invalid_argument("biquad a0 coefficient must be non-zero");
  }
  return Biquad(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0);
}

Biquad Biquad::peaking(const float sample_rate, const float frequency_hz, const float quality,
                       const float gain_db) {
  const float omega = 2.0F * std::numbers::pi_v<float> * frequency_hz / sample_rate;
  const float alpha = std::sin(omega) / (2.0F * quality);
  const float amplitude = std::pow(10.0F, gain_db / 40.0F);
  const float cosine = std::cos(omega);
  return normalized(1.0F + (alpha * amplitude), -2.0F * cosine,
                    1.0F - (alpha * amplitude), 1.0F + (alpha / amplitude),
                    -2.0F * cosine, 1.0F - (alpha / amplitude));
}

Biquad Biquad::high_pass(const float sample_rate, const float frequency_hz, const float quality) {
  const float omega = 2.0F * std::numbers::pi_v<float> * frequency_hz / sample_rate;
  const float alpha = std::sin(omega) / (2.0F * quality);
  const float cosine = std::cos(omega);
  return normalized((1.0F + cosine) / 2.0F, -(1.0F + cosine),
                    (1.0F + cosine) / 2.0F, 1.0F + alpha, -2.0F * cosine,
                    1.0F - alpha);
}

Biquad Biquad::low_pass(const float sample_rate, const float frequency_hz, const float quality) {
  const float omega = 2.0F * std::numbers::pi_v<float> * frequency_hz / sample_rate;
  const float alpha = std::sin(omega) / (2.0F * quality);
  const float cosine = std::cos(omega);
  return normalized((1.0F - cosine) / 2.0F, 1.0F - cosine,
                    (1.0F - cosine) / 2.0F, 1.0F + alpha, -2.0F * cosine,
                    1.0F - alpha);
}

void Biquad::reset(const std::size_t channels) { states_.assign(channels, {}); }

void Biquad::process(AudioBlock& block) noexcept {
  if (states_.size() != block.format().channels) {
    states_.assign(block.format().channels, {});
  }
  for (std::size_t channel_index = 0; channel_index < block.format().channels; ++channel_index) {
    State& state = states_[channel_index];
    for (float& input_output : block.channel(channel_index)) {
      const float input = input_output;
      const float output = (b0_ * input) + (b1_ * state.x1) + (b2_ * state.x2) -
                           (a1_ * state.y1) - (a2_ * state.y2);
      state.x2 = state.x1;
      state.x1 = input;
      state.y2 = state.y1;
      state.y1 = output;
      input_output = output;
    }
  }
}

Compressor::Compressor(const float sample_rate) : sample_rate_(sample_rate) {
  if (sample_rate <= 0.0F) {
    throw std::invalid_argument("compressor sample rate must be positive");
  }
}

void Compressor::configure(const CompressorSettings settings) noexcept { settings_ = settings; }

void Compressor::reset() noexcept { envelope_ = 0.0F; }

void Compressor::process(AudioBlock& block) noexcept {
  const float attack = std::exp(-1.0F / (0.001F * settings_.attack_ms * sample_rate_));
  const float release = std::exp(-1.0F / (0.001F * settings_.release_ms * sample_rate_));
  const float makeup = db_to_linear(settings_.makeup_db);
  for (std::size_t frame = 0; frame < block.frame_count(); ++frame) {
    float detector = 0.0F;
    for (std::size_t channel = 0; channel < block.format().channels; ++channel) {
      detector = std::max(detector, std::abs(block.channel(channel)[frame]));
    }
    const float coefficient = detector > envelope_ ? attack : release;
    envelope_ = (coefficient * envelope_) + ((1.0F - coefficient) * detector);
    const float input_db = linear_to_db(envelope_);
    const float compressed_db = input_db > settings_.threshold_db
                                    ? settings_.threshold_db +
                                          ((input_db - settings_.threshold_db) /
                                           std::max(settings_.ratio, 1.0F))
                                    : input_db;
    const float gain = db_to_linear(compressed_db - input_db) * makeup;
    for (std::size_t channel = 0; channel < block.format().channels; ++channel) {
      block.channel(channel)[frame] *= gain;
    }
  }
}

LookaheadFreeLimiter::LookaheadFreeLimiter(const float ceiling_db) { set_ceiling(ceiling_db); }

void LookaheadFreeLimiter::set_ceiling(const float ceiling_db) noexcept {
  ceiling_linear_ = db_to_linear(std::min(ceiling_db, 0.0F));
}

void LookaheadFreeLimiter::process(AudioBlock& block) const noexcept {
  for (std::size_t channel = 0; channel < block.format().channels; ++channel) {
    for (float& sample : block.channel(channel)) {
      sample = std::clamp(sample, -ceiling_linear_, ceiling_linear_);
    }
  }
}

LevelReading measure_levels(const AudioBlock& block) {
  LevelReading result;
  result.peak.resize(block.format().channels, 0.0F);
  result.rms.resize(block.format().channels, 0.0F);
  for (std::size_t channel_index = 0; channel_index < block.format().channels; ++channel_index) {
    double squares = 0.0;
    for (const float sample : block.channel(channel_index)) {
      result.peak[channel_index] = std::max(result.peak[channel_index], std::abs(sample));
      squares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    if (block.frame_count() > 0U) {
      result.rms[channel_index] =
          static_cast<float>(std::sqrt(squares / static_cast<double>(block.frame_count())));
    }
  }
  return result;
}

} // namespace video_editor::audio
