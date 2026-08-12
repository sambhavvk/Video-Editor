// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"

#include <array>
#include <cstddef>
#include <vector>

namespace video_editor::audio {

void apply_gain(AudioBlock& block, float linear_gain) noexcept;
void apply_stereo_pan(AudioBlock& block, float pan) noexcept;

class Biquad {
public:
  static Biquad peaking(float sample_rate, float frequency_hz, float quality, float gain_db);
  static Biquad high_pass(float sample_rate, float frequency_hz, float quality);
  static Biquad low_pass(float sample_rate, float frequency_hz, float quality);

  void reset(std::size_t channels);
  void process(AudioBlock& block) noexcept;

private:
  Biquad(float b0, float b1, float b2, float a1, float a2) noexcept;
  [[nodiscard]] static Biquad normalized(float b0, float b1, float b2, float a0,
                                         float a1, float a2);

  struct State {
    float x1{0.0F};
    float x2{0.0F};
    float y1{0.0F};
    float y2{0.0F};
  };

  float b0_{1.0F};
  float b1_{0.0F};
  float b2_{0.0F};
  float a1_{0.0F};
  float a2_{0.0F};
  std::vector<State> states_;
};

struct CompressorSettings {
  float threshold_db{-18.0F};
  float ratio{4.0F};
  float attack_ms{10.0F};
  float release_ms{100.0F};
  float makeup_db{0.0F};
};

class Compressor {
public:
  explicit Compressor(float sample_rate = 48'000.0F);
  void configure(CompressorSettings settings) noexcept;
  void reset() noexcept;
  void process(AudioBlock& block) noexcept;

private:
  float sample_rate_;
  CompressorSettings settings_;
  float envelope_{0.0F};
};

class LookaheadFreeLimiter {
public:
  explicit LookaheadFreeLimiter(float ceiling_db = -1.0F);
  void set_ceiling(float ceiling_db) noexcept;
  void process(AudioBlock& block) const noexcept;

private:
  float ceiling_linear_;
};

struct LevelReading {
  std::vector<float> peak;
  std::vector<float> rms;
};

[[nodiscard]] LevelReading measure_levels(const AudioBlock& block);

} // namespace video_editor::audio
