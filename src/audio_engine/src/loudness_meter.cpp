// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/loudness_meter.h"

#include <ebur128.h>

#include <cmath>
#include <limits>
#include <utility>

namespace video_editor::audio {

class LoudnessMeter::Impl {
public:
  explicit Impl(const AudioFormat format_value) : format(format_value) { initialise(); }
  ~Impl() {
    if (state != nullptr) {
      ebur128_destroy(&state);
    }
  }

  void initialise() {
    state = ebur128_init(format.channels, format.sample_rate,
                         EBUR128_MODE_I | EBUR128_MODE_S | EBUR128_MODE_SAMPLE_PEAK);
  }

  AudioFormat format;
  ebur128_state* state{nullptr};
};

LoudnessMeter::LoudnessMeter(const AudioFormat format) : impl_(std::make_unique<Impl>(format)) {}
LoudnessMeter::~LoudnessMeter() = default;
LoudnessMeter::LoudnessMeter(LoudnessMeter&&) noexcept = default;
LoudnessMeter& LoudnessMeter::operator=(LoudnessMeter&&) noexcept = default;

bool LoudnessMeter::valid() const noexcept { return impl_ != nullptr && impl_->state != nullptr; }

bool LoudnessMeter::add(const AudioBlock& block) noexcept {
  if (!valid() || block.format().channels != impl_->format.channels ||
      block.format().sample_rate != impl_->format.sample_rate) {
    return false;
  }
  const std::vector<float> samples = block.interleaved();
  return ebur128_add_frames_float(impl_->state, samples.data(), block.frame_count()) == EBUR128_SUCCESS;
}

LoudnessReading LoudnessMeter::reading() const noexcept {
  LoudnessReading result;
  if (!valid()) {
    return result;
  }
  double value = 0.0;
  if (ebur128_loudness_global(impl_->state, &value) == EBUR128_SUCCESS && std::isfinite(value)) {
    result.integrated_lufs = value;
  }
  if (ebur128_loudness_shortterm(impl_->state, &value) == EBUR128_SUCCESS && std::isfinite(value)) {
    result.short_term_lufs = value;
  }
  result.sample_peak_dbfs.reserve(impl_->format.channels);
  for (unsigned channel = 0; channel < impl_->format.channels; ++channel) {
    if (ebur128_sample_peak(impl_->state, channel, &value) == EBUR128_SUCCESS && value > 0.0) {
      result.sample_peak_dbfs.push_back(20.0 * std::log10(value));
    } else {
      result.sample_peak_dbfs.push_back(-std::numeric_limits<double>::infinity());
    }
  }
  return result;
}

void LoudnessMeter::reset() noexcept {
  if (!impl_) {
    return;
  }
  const AudioFormat format = impl_->format;
  impl_ = std::make_unique<Impl>(format);
}

} // namespace video_editor::audio
