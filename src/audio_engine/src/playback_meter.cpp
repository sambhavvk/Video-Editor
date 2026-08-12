// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/playback_meter.h"

#include <algorithm>
#include <cmath>

namespace video_editor::audio {

void PlaybackMeter::process(const float* interleaved, const std::size_t frame_count,
                            const std::uint32_t channels) noexcept {
  if (interleaved == nullptr || frame_count == 0U || channels == 0U || channels > 2U) {
    return;
  }
  channels_.store(channels, std::memory_order_release);
  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    for (std::uint32_t c = 0; c < channels; ++c) {
      const float sample = interleaved[(frame * channels) + c];
      const float abs_sample = std::abs(sample);
      // Peak: atomic max.
      float current_peak = peak_[c].load(std::memory_order_relaxed);
      while (abs_sample > current_peak) {
        if (peak_[c].compare_exchange_weak(current_peak, abs_sample,
                                           std::memory_order_relaxed)) {
          break;
        }
      }
      // RMS: accumulate sum of squares.
      const double sq = static_cast<double>(sample) * static_cast<double>(sample);
      double current = sum_squares_[c].load(std::memory_order_relaxed);
      sum_squares_[c].store(current + sq, std::memory_order_relaxed);
    }
  }
  sample_count_.fetch_add(frame_count, std::memory_order_relaxed);
}

PlaybackMeter::Reading PlaybackMeter::read() const noexcept {
  Reading result;
  const std::uint32_t channels = channels_.load(std::memory_order_acquire);
  const std::uint64_t count = sample_count_.exchange(0, std::memory_order_acq_rel);
  result.sample_count = count;
  for (std::uint32_t c = 0; c < channels && c < 2U; ++c) {
    result.peak[c] = peak_[c].exchange(0.0F, std::memory_order_acq_rel);
    const double sum_sq = sum_squares_[c].exchange(0.0, std::memory_order_acq_rel);
    if (count > 0U) {
      result.rms[c] = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count)));
    } else {
      result.rms[c] = 0.0F;
    }
  }
  return result;
}

void PlaybackMeter::reset() noexcept {
  for (auto& p : peak_) {
    p.store(0.0F, std::memory_order_release);
  }
  for (auto& s : sum_squares_) {
    s.store(0.0, std::memory_order_release);
  }
  sample_count_.store(0, std::memory_order_release);
}

} // namespace video_editor::audio
