// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/loudness_meter.h"

#include <ebur128.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace video_editor::audio {

class LoudnessMeter::Impl {
public:
  explicit Impl(const AudioFormat format_value) : format(format_value) {
    initialise();
  }
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

bool LoudnessMeter::valid() const noexcept {
  return impl_ != nullptr && impl_->state != nullptr;
}

bool LoudnessMeter::add(const AudioBlock& block) noexcept {
  if (!valid() || block.format().channels != impl_->format.channels ||
      block.format().sample_rate != impl_->format.sample_rate) {
    return false;
  }
  const std::vector<float> samples = block.interleaved();
  return ebur128_add_frames_float(impl_->state, samples.data(), block.frame_count()) ==
         EBUR128_SUCCESS;
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

void RealtimeLoudnessMeter::process(const float* interleaved, const std::size_t frame_count,
                                    const std::uint32_t channels) noexcept {
  if (interleaved == nullptr || frame_count == 0U || channels == 0U) {
    return;
  }
  channels_.store(channels, std::memory_order_release);
  double sum = 0.0;
  float peak = 0.0F;
  const std::size_t sample_count = frame_count * static_cast<std::size_t>(channels);
  for (std::size_t index = 0; index < sample_count; ++index) {
    const float sample = interleaved[index];
    peak = std::max(peak, std::abs(sample));
    sum += static_cast<double>(sample) * static_cast<double>(sample);
  }

  double current = sum_squares_.load(std::memory_order_relaxed);
  while (!sum_squares_.compare_exchange_weak(current, current + sum, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
  }
  float current_peak = sample_peak_.load(std::memory_order_relaxed);
  while (peak > current_peak &&
         !sample_peak_.compare_exchange_weak(current_peak, peak, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
  }
  sample_count_.fetch_add(frame_count, std::memory_order_relaxed);
}

RealtimeLoudnessMeter::Reading RealtimeLoudnessMeter::read() const noexcept {
  const std::uint64_t frames = sample_count_.exchange(0U, std::memory_order_acq_rel);
  const double sum = sum_squares_.exchange(0.0, std::memory_order_acq_rel);
  const float peak = sample_peak_.exchange(0.0F, std::memory_order_acq_rel);
  const std::uint32_t channels = channels_.load(std::memory_order_acquire);
  Reading result{.sample_peak = peak, .sample_count = frames, .channels = channels};
  if (frames != 0U && channels != 0U && sum > 0.0 && std::isfinite(sum)) {
    // The approximation uses the mean over interleaved channels. The -0.691
    // dB BS.1770 calibration offset is retained so polling meters line up with
    // the offline ebur128 reading for steady material.
    constexpr double kBs1770OffsetDb = -0.691;
    result.integrated_lufs =
        (10.0 * std::log10(sum / static_cast<double>(frames * channels))) + kBs1770OffsetDb;
  }
  return result;
}

void RealtimeLoudnessMeter::reset() noexcept {
  sum_squares_.store(0.0, std::memory_order_release);
  sample_peak_.store(0.0F, std::memory_order_release);
  sample_count_.store(0U, std::memory_order_release);
  channels_.store(0U, std::memory_order_release);
}

class RealtimeLoudnessAnalyzer::Impl final {
public:
  struct Slot final {
    std::vector<float> samples;
    std::size_t frame_count{0};
    std::uint32_t channels{0};
    std::uint64_t generation{0};
  };

  Impl(const AudioFormat format_value,
       const RealtimeLoudnessAnalysisConfiguration configuration_value)
      : format(format_value), configuration(configuration_value) {
    if (format.sample_rate == 0U || format.channels == 0U || configuration.queue_blocks == 0U ||
        configuration.maximum_block_frames == 0U ||
        configuration.maximum_block_frames >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(format.channels)) {
      throw std::invalid_argument("realtime loudness analyzer configuration is invalid");
    }
    slots.reserve(configuration.queue_blocks);
    for (std::size_t index = 0; index < configuration.queue_blocks; ++index) {
      slots.push_back(
          Slot{.samples = std::vector<float>(configuration.maximum_block_frames *
                                             static_cast<std::size_t>(format.channels))});
    }
    initialise_state();
    worker = std::jthread([this](const std::stop_token token) { worker_main(token); });
  }

  ~Impl() {
    worker.request_stop();
    worker.join();
    destroy_state();
  }

  void initialise_state() {
    destroy_state();
    state =
        ebur128_init(format.channels, format.sample_rate,
                     EBUR128_MODE_I | EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_SAMPLE_PEAK);
  }

  void destroy_state() noexcept {
    if (state != nullptr) {
      ebur128_destroy(&state);
    }
  }

  void publish_metric(const double value, std::atomic<double>& destination,
                      std::atomic<bool>& valid) noexcept {
    if (std::isfinite(value)) {
      destination.store(value, std::memory_order_release);
      valid.store(true, std::memory_order_release);
    }
  }

  void analyze(Slot& slot) noexcept {
    const std::uint64_t slot_generation = slot.generation;
    if (state == nullptr || slot.channels != format.channels || slot.frame_count == 0U ||
        slot_generation != generation.load(std::memory_order_acquire)) {
      return;
    }
    if (ebur128_add_frames_float(state, slot.samples.data(), slot.frame_count) != EBUR128_SUCCESS) {
      return;
    }
    // A reset may race the worker between the libebur128 call and publication.
    // The generation remains stale in that case; the next current-generation
    // block causes the worker to recreate the state before publishing again.
    if (slot_generation != generation.load(std::memory_order_acquire)) {
      return;
    }
    double value = 0.0;
    if (ebur128_loudness_momentary(state, &value) == EBUR128_SUCCESS) {
      publish_metric(value, momentary_lufs, momentary_valid);
    }
    if (ebur128_loudness_shortterm(state, &value) == EBUR128_SUCCESS) {
      publish_metric(value, short_term_lufs, short_term_valid);
    }
    if (ebur128_loudness_global(state, &value) == EBUR128_SUCCESS) {
      publish_metric(value, integrated_lufs, integrated_valid);
    }
    analyzed_frames.fetch_add(slot.frame_count, std::memory_order_relaxed);
    version.fetch_add(1U, std::memory_order_release);
    published_generation.store(slot_generation, std::memory_order_release);
  }

  void worker_main(const std::stop_token token) noexcept {
    std::uint64_t worker_generation = generation.load(std::memory_order_acquire);
    while (!token.stop_requested()) {
      const std::uint64_t read = read_index.load(std::memory_order_relaxed);
      if (read == write_index.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (read_index.load(std::memory_order_acquire) != read) {
        // A control-thread reset advanced the consumer cursor while this
        // worker was observing the previous cursor.
        continue;
      }
      Slot& slot = slots[static_cast<std::size_t>(read % slots.size())];
      const std::uint64_t current_generation = generation.load(std::memory_order_acquire);
      if (slot.generation != current_generation) {
        // Reset invalidates all queued blocks from the previous generation.
        // They are consumed without touching libebur128 or the published
        // metrics, keeping reset deterministic and bounded.
        std::uint64_t expected = read;
        static_cast<void>(read_index.compare_exchange_strong(
            expected, read + 1U, std::memory_order_release, std::memory_order_relaxed));
        continue;
      }
      if (current_generation != worker_generation) {
        initialise_state();
        worker_generation = current_generation;
        clear_metrics();
      }
      analyze(slot);
      std::uint64_t expected = read;
      static_cast<void>(read_index.compare_exchange_strong(
          expected, read + 1U, std::memory_order_release, std::memory_order_relaxed));
    }
  }

  void clear_metrics() noexcept {
    version.store(0U, std::memory_order_release);
    analyzed_frames.store(0U, std::memory_order_release);
    momentary_lufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_release);
    short_term_lufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_release);
    integrated_lufs.store(-std::numeric_limits<double>::infinity(), std::memory_order_release);
    momentary_valid.store(false, std::memory_order_release);
    short_term_valid.store(false, std::memory_order_release);
    integrated_valid.store(false, std::memory_order_release);
  }

  [[nodiscard]] bool submit(const float* interleaved, const std::size_t frame_count,
                            const std::uint32_t channels) noexcept {
    if (interleaved == nullptr || frame_count == 0U || channels != format.channels ||
        frame_count > configuration.maximum_block_frames) {
      dropped_blocks.fetch_add(1U, std::memory_order_relaxed);
      return false;
    }
    const std::uint64_t write = write_index.load(std::memory_order_relaxed);
    const std::uint64_t read = read_index.load(std::memory_order_acquire);
    if (write - read >= slots.size()) {
      dropped_blocks.fetch_add(1U, std::memory_order_relaxed);
      return false;
    }
    Slot& slot = slots[static_cast<std::size_t>(write % slots.size())];
    slot.frame_count = frame_count;
    slot.channels = channels;
    slot.generation = generation.load(std::memory_order_acquire);
    const std::size_t sample_count = frame_count * static_cast<std::size_t>(channels);
    std::copy_n(interleaved, sample_count, slot.samples.data());
    write_index.store(write + 1U, std::memory_order_release);
    return true;
  }

  [[nodiscard]] Reading read() const noexcept {
    const std::uint64_t current_generation = generation.load(std::memory_order_acquire);
    const std::uint64_t published = published_generation.load(std::memory_order_acquire);
    const std::uint64_t read_index_value = read_index.load(std::memory_order_acquire);
    const std::uint64_t write_index_value = write_index.load(std::memory_order_acquire);
    return Reading{
        .version = version.load(std::memory_order_acquire),
        .momentary_lufs = momentary_lufs.load(std::memory_order_acquire),
        .short_term_lufs = short_term_lufs.load(std::memory_order_acquire),
        .integrated_lufs = integrated_lufs.load(std::memory_order_acquire),
        .momentary_valid = momentary_valid.load(std::memory_order_acquire),
        .short_term_valid = short_term_valid.load(std::memory_order_acquire),
        .integrated_valid = integrated_valid.load(std::memory_order_acquire),
        .stale = current_generation != published || read_index_value != write_index_value ||
                 dropped_blocks.load(std::memory_order_acquire) != 0U,
        .analyzed_frames = analyzed_frames.load(std::memory_order_acquire),
        .dropped_blocks = dropped_blocks.load(std::memory_order_acquire),
        .channels = format.channels,
    };
  }

  void reset() noexcept {
    generation.fetch_add(1U, std::memory_order_acq_rel);
    // `submit()` is single-producer by contract. The producer must be
    // quiesced while reset invalidates queued blocks. Do not advance the
    // consumer cursor here: the worker may still be reading the slot at that
    // cursor, and making it immediately reusable would let the producer
    // overwrite that storage concurrently. The worker drains old-generation
    // slots without analyzing them and is the sole owner of read_index.
    dropped_blocks.store(0U, std::memory_order_release);
    clear_metrics();
  }

  AudioFormat format;
  RealtimeLoudnessAnalysisConfiguration configuration;
  std::vector<Slot> slots;
  ebur128_state* state{nullptr};
  std::jthread worker;
  std::atomic<std::uint64_t> write_index{0};
  std::atomic<std::uint64_t> read_index{0};
  std::atomic<std::uint64_t> generation{0};
  std::atomic<std::uint64_t> published_generation{0};
  std::atomic<std::uint64_t> version{0};
  std::atomic<std::uint64_t> analyzed_frames{0};
  std::atomic<std::uint64_t> dropped_blocks{0};
  std::atomic<double> momentary_lufs{-std::numeric_limits<double>::infinity()};
  std::atomic<double> short_term_lufs{-std::numeric_limits<double>::infinity()};
  std::atomic<double> integrated_lufs{-std::numeric_limits<double>::infinity()};
  std::atomic<bool> momentary_valid{false};
  std::atomic<bool> short_term_valid{false};
  std::atomic<bool> integrated_valid{false};
};

RealtimeLoudnessAnalyzer::RealtimeLoudnessAnalyzer(
    const AudioFormat format, const RealtimeLoudnessAnalysisConfiguration configuration)
    : impl_(std::make_unique<Impl>(format, configuration)) {}

RealtimeLoudnessAnalyzer::~RealtimeLoudnessAnalyzer() = default;

bool RealtimeLoudnessAnalyzer::submit(const float* interleaved, const std::size_t frame_count,
                                      const std::uint32_t channels) noexcept {
  return impl_->submit(interleaved, frame_count, channels);
}

RealtimeLoudnessAnalyzer::Reading RealtimeLoudnessAnalyzer::read() const noexcept {
  return impl_->read();
}

void RealtimeLoudnessAnalyzer::reset() noexcept {
  impl_->reset();
}

} // namespace video_editor::audio
