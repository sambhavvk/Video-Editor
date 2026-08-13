// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace video_editor::audio {

struct LoudnessReading {
  std::optional<double> integrated_lufs;
  std::optional<double> short_term_lufs;
  std::vector<double> sample_peak_dbfs;
};

class LoudnessMeter {
public:
  explicit LoudnessMeter(AudioFormat format);
  ~LoudnessMeter();
  LoudnessMeter(LoudnessMeter&&) noexcept;
  LoudnessMeter& operator=(LoudnessMeter&&) noexcept;
  LoudnessMeter(const LoudnessMeter&) = delete;
  LoudnessMeter& operator=(const LoudnessMeter&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool add(const AudioBlock& block) noexcept;
  [[nodiscard]] LoudnessReading reading() const noexcept;
  void reset() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Allocation-free, callback-safe loudness telemetry. This is intentionally a
// streaming approximation of integrated LUFS: it accumulates mean-square
// energy and applies the BS.1770 reference offset. Offline normalization uses
// LoudnessMeter above for the full gated EBU-R128 calculation. `process()` is
// single-writer (the audio callback); `read()` may run concurrently on a
// polling/UI thread and atomically consumes the current window.
class RealtimeLoudnessMeter final {
public:
  RealtimeLoudnessMeter() = default;

  RealtimeLoudnessMeter(const RealtimeLoudnessMeter&) = delete;
  RealtimeLoudnessMeter& operator=(const RealtimeLoudnessMeter&) = delete;

  void process(const float* interleaved, std::size_t frame_count, std::uint32_t channels) noexcept;

  struct Reading final {
    double integrated_lufs{-std::numeric_limits<double>::infinity()};
    float sample_peak{0.0F};
    std::uint64_t sample_count{0};
    std::uint32_t channels{0};
  };

  [[nodiscard]] Reading read() const noexcept;
  void reset() noexcept;

private:
  mutable std::atomic<double> sum_squares_{0.0};
  mutable std::atomic<float> sample_peak_{0.0F};
  mutable std::atomic<std::uint64_t> sample_count_{0};
  mutable std::atomic<std::uint32_t> channels_{0};
};

struct RealtimeLoudnessAnalysisConfiguration final {
  // Fixed-size SPSC queue owned by the realtime producer and analysis worker.
  std::size_t queue_blocks{64};
  std::size_t maximum_block_frames{2'048};
};

// Authoritative realtime EBU-R128 analysis. `submit()` is intended for one
// realtime producer (the audio callback): it only copies into a preallocated
// bounded SPSC queue and never allocates, locks, or waits. Callers must not
// invoke submit concurrently from multiple producer threads. A dedicated
// worker owns libebur128 state
// and publishes immutable metric fields atomically. Dropped blocks make a
// reading stale until reset; validity is tracked independently because
// momentary/short-term/global windows become valid at different times.
class RealtimeLoudnessAnalyzer final {
public:
  struct Reading final {
    std::uint64_t version{0};
    double momentary_lufs{-std::numeric_limits<double>::infinity()};
    double short_term_lufs{-std::numeric_limits<double>::infinity()};
    double integrated_lufs{-std::numeric_limits<double>::infinity()};
    bool momentary_valid{false};
    bool short_term_valid{false};
    bool integrated_valid{false};
    bool stale{true};
    std::uint64_t analyzed_frames{0};
    std::uint64_t dropped_blocks{0};
    std::uint32_t channels{0};
  };

  explicit RealtimeLoudnessAnalyzer(AudioFormat format = {},
                                    RealtimeLoudnessAnalysisConfiguration configuration = {});
  ~RealtimeLoudnessAnalyzer();
  RealtimeLoudnessAnalyzer(const RealtimeLoudnessAnalyzer&) = delete;
  RealtimeLoudnessAnalyzer& operator=(const RealtimeLoudnessAnalyzer&) = delete;

  [[nodiscard]] bool submit(const float* interleaved, std::size_t frame_count,
                            std::uint32_t channels) noexcept;
  [[nodiscard]] Reading read() const noexcept;
  // Control-thread operation. Quiesce the single submit producer before
  // calling reset; playback does this after closing the callback gate.
  void reset() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::audio
