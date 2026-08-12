// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace video_editor::audio {

// A lock-free, allocation-free peak/RMS meter designed for the realtime
// playback callback. The callback thread calls `process()` on each output
// buffer; a reader thread calls `read()` to get the latest levels and reset
// the peak hold. All state is atomic; no locks are taken in the callback path.
//
// Peak readings are sample-peak (absolute maximum). RMS is computed over the
// samples processed since the last `read()`. Both are reported per channel in
// linear amplitude (0..1). The reader converts to dBFS as needed.
class PlaybackMeter final {
public:
  PlaybackMeter() = default;

  // Process interleaved float samples from the callback. `frame_count` is the
  // number of frames; `channels` is the channel count. Must be called from one
  // thread only (the audio callback). noexcept and allocation-free.
  void process(const float* interleaved, std::size_t frame_count,
               std::uint32_t channels) noexcept;

  // Read the latest levels and reset the accumulators for the next window.
  // Returns per-channel peak (linear) and RMS (linear). Safe to call from any
  // thread. If no samples have been processed since the last read, returns
  // zeros.
  struct Reading {
    float peak[2]{0.0F, 0.0F};
    float rms[2]{0.0F, 0.0F};
    std::uint64_t sample_count{0};
  };
  [[nodiscard]] Reading read() const noexcept;

  void reset() noexcept;

private:
  // Accumulators written by the callback thread. Mutable because read()
  // exchanges them (resetting accumulators) despite being const-callable.
  mutable std::atomic<float> peak_[2]{{0.0F}, {0.0F}};
  mutable std::atomic<double> sum_squares_[2]{{0.0}, {0.0}};
  mutable std::atomic<std::uint64_t> sample_count_{0};
  std::atomic<std::uint32_t> channels_{2};
};

} // namespace video_editor::audio
