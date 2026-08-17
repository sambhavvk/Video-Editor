// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/edit_model/result.h"
#include "video_editor/edit_model/time.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::audio_render {

struct SilenceRange final {
  std::int64_t start_sample{0};
  std::int64_t end_sample{0};
  [[nodiscard]] edit::TimeRange time_range() const {
    return edit::TimeRange(edit::Time(start_sample, kTimelineAudioSampleRate),
                           edit::Time(end_sample - start_sample, kTimelineAudioSampleRate));
  }
  friend bool operator==(const SilenceRange&, const SilenceRange&) = default;
};

struct SilenceOptions final {
  std::size_t analysis_window_samples{480}; // 10 ms at the canonical rate
  std::size_t minimum_silence_samples{2'400};
  std::size_t merge_gap_samples{0};
  // Padding/hangover is intentionally proposal-layer policy; this detector
  // reports only the exact analyzed half-open windows.
  float rms_threshold{0.001F};
  float peak_threshold{0.01F};
};

enum class SilenceErrorCode { InvalidFormat, InvalidOptions, InvalidAudio };
struct SilenceError final {
  SilenceErrorCode code{SilenceErrorCode::InvalidFormat};
  std::string message;
};
using SilenceResult = edit::Result<std::vector<SilenceRange>, SilenceError>;

// Incremental detector used by bounded analysis jobs. It retains only the current
// analysis window and the current silence run, so callers can feed arbitrarily
// long selections without constructing an AudioBlock for the whole range.
class SilenceAccumulator final {
public:
  explicit SilenceAccumulator(SilenceOptions options = {});

  SilenceAccumulator(const SilenceAccumulator&) = delete;
  SilenceAccumulator& operator=(const SilenceAccumulator&) = delete;

  [[nodiscard]] bool add(const audio::AudioBlock& block);
  [[nodiscard]] SilenceResult finish();
  [[nodiscard]] const std::optional<SilenceError>& error() const noexcept {
    return error_;
  }

private:
  void consumeWindow(bool silent, std::int64_t start, std::int64_t end);
  void setError(SilenceError error);

  SilenceOptions options_;
  std::vector<SilenceRange> result_;
  std::optional<SilenceRange> active_run_;
  std::optional<SilenceRange> pending_range_;
  std::optional<SilenceError> error_;
  std::int64_t expected_start_{0};
  std::int64_t window_start_{0};
  std::size_t window_samples_{0};
  std::uint32_t channels_{0};
  long double window_sum_{0.0L};
  float window_peak_{0.0F};
  bool has_input_{false};
  bool finished_{false};
};

// Analyses exact 48 kHz AudioBlock samples. Returned ranges are absolute,
// half-open sample intervals and are deterministic for a fixed block/options.
[[nodiscard]] SilenceResult detectSilence(const audio::AudioBlock& block,
                                          const SilenceOptions& options = {});

} // namespace video_editor::audio_render
