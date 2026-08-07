// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_engine/audio_block.h"

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

} // namespace video_editor::audio

