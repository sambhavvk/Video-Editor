// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/silence_detector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace video_editor::audio_render {
namespace {

[[nodiscard]] SilenceError invalidOptions() {
  return {SilenceErrorCode::InvalidOptions, "silence thresholds and windows are invalid"};
}

[[nodiscard]] bool validOptions(const SilenceOptions& options) {
  return options.analysis_window_samples != 0U && options.minimum_silence_samples != 0U &&
         std::isfinite(options.rms_threshold) && std::isfinite(options.peak_threshold) &&
         options.rms_threshold >= 0.0F && options.peak_threshold >= 0.0F;
}

} // namespace

SilenceAccumulator::SilenceAccumulator(SilenceOptions options) : options_(options) {
  if (!validOptions(options_)) {
    error_ = invalidOptions();
  }
}

void SilenceAccumulator::setError(SilenceError error) {
  if (!error_.has_value()) {
    error_ = std::move(error);
  }
}

void SilenceAccumulator::consumeWindow(const bool silent, const std::int64_t start,
                                       const std::int64_t end) {
  if (silent) {
    if (active_run_.has_value() && active_run_->end_sample == start) {
      active_run_->end_sample = end;
    } else {
      if (active_run_.has_value() &&
          active_run_->end_sample - active_run_->start_sample >=
              static_cast<std::int64_t>(options_.minimum_silence_samples)) {
        pending_range_ = active_run_;
      }
      active_run_ = SilenceRange{start, end};
    }
    return;
  }
  if (active_run_.has_value()) {
    if (active_run_->end_sample - active_run_->start_sample >=
        static_cast<std::int64_t>(options_.minimum_silence_samples)) {
      if (pending_range_.has_value() && active_run_->start_sample - pending_range_->end_sample <=
                                            static_cast<std::int64_t>(options_.merge_gap_samples)) {
        pending_range_->end_sample = active_run_->end_sample;
      } else {
        if (pending_range_.has_value()) {
          result_.push_back(*pending_range_);
        }
        pending_range_ = active_run_;
      }
    }
    active_run_.reset();
  }
}

bool SilenceAccumulator::add(const audio::AudioBlock& block) {
  if (error_.has_value() || finished_) {
    return false;
  }
  if (block.format().sample_rate != kTimelineAudioSampleRate || block.format().channels == 0U ||
      block.format().channels > 2U) {
    setError({SilenceErrorCode::InvalidFormat,
              "silence detection requires exact 48 kHz mono/stereo audio"});
    return false;
  }
  if (block.frame_count() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
      block.start_sample() > std::numeric_limits<std::int64_t>::max() -
                                 static_cast<std::int64_t>(block.frame_count())) {
    setError({SilenceErrorCode::InvalidAudio, "audio block sample range overflows int64"});
    return false;
  }
  for (std::size_t channel = 0; channel < block.format().channels; ++channel) {
    if (block.channel(channel).size() != block.frame_count()) {
      setError({SilenceErrorCode::InvalidAudio, "audio channel length does not match frame count"});
      return false;
    }
  }
  if (has_input_ && block.start_sample() != expected_start_) {
    setError({SilenceErrorCode::InvalidAudio, "audio blocks are not contiguous"});
    return false;
  }
  if (has_input_ && block.format().channels != channels_) {
    setError({SilenceErrorCode::InvalidFormat,
              "silence detection audio blocks must keep one channel layout"});
    return false;
  }
  if (!has_input_) {
    expected_start_ = block.start_sample();
    window_start_ = block.start_sample();
    channels_ = block.format().channels;
    has_input_ = true;
  }

  for (std::size_t index = 0; index < block.frame_count(); ++index) {
    for (std::size_t channel = 0; channel < block.format().channels; ++channel) {
      const float sample = block.channel(channel)[index];
      if (!std::isfinite(sample)) {
        setError({SilenceErrorCode::InvalidAudio, "audio contains a non-finite sample"});
        return false;
      }
      window_peak_ = std::max(window_peak_, std::abs(sample));
      window_sum_ += static_cast<long double>(sample) * static_cast<long double>(sample);
    }
    ++window_samples_;
    ++expected_start_;
    if (window_samples_ == options_.analysis_window_samples) {
      const auto denominator = static_cast<long double>(
          window_samples_ * static_cast<std::size_t>(block.format().channels));
      const float rms = static_cast<float>(std::sqrt(window_sum_ / denominator));
      consumeWindow(rms <= options_.rms_threshold && window_peak_ <= options_.peak_threshold,
                    window_start_, expected_start_);
      window_start_ = expected_start_;
      window_samples_ = 0;
      window_sum_ = 0.0L;
      window_peak_ = 0.0F;
    }
  }
  return !error_.has_value();
}

SilenceResult SilenceAccumulator::finish() {
  if (finished_) {
    return error_.has_value() ? SilenceResult::failure(*error_) : SilenceResult::success(result_);
  }
  finished_ = true;
  if (error_.has_value()) {
    return SilenceResult::failure(*error_);
  }
  if (window_samples_ != 0U) {
    const auto denominator = static_cast<long double>(window_samples_) * channels_;
    const float rms = static_cast<float>(std::sqrt(window_sum_ / denominator));
    consumeWindow(rms <= options_.rms_threshold && window_peak_ <= options_.peak_threshold,
                  window_start_, expected_start_);
    window_samples_ = 0;
  }
  if (active_run_.has_value() && active_run_->end_sample - active_run_->start_sample >=
                                     static_cast<std::int64_t>(options_.minimum_silence_samples)) {
    if (pending_range_.has_value() && active_run_->start_sample - pending_range_->end_sample <=
                                          static_cast<std::int64_t>(options_.merge_gap_samples)) {
      pending_range_->end_sample = active_run_->end_sample;
    } else {
      if (pending_range_.has_value()) {
        result_.push_back(*pending_range_);
      }
      pending_range_ = active_run_;
    }
  }
  active_run_.reset();
  if (pending_range_.has_value()) {
    result_.push_back(*pending_range_);
    pending_range_.reset();
  }
  return SilenceResult::success(result_);
}

SilenceResult detectSilence(const audio::AudioBlock& block, const SilenceOptions& options) {
  SilenceAccumulator accumulator(options);
  if (!accumulator.add(block)) {
    return SilenceResult::failure(*accumulator.error());
  }
  return accumulator.finish();
}

} // namespace video_editor::audio_render
