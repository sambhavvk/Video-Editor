// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/shuttle_pitch_correct.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace video_editor::audio {
namespace {

constexpr std::size_t kFrameSize = 2'048U;
constexpr std::size_t kSynthesisHop = 512U;
constexpr std::size_t kSearchRadius = 256U;

[[nodiscard]] std::vector<float> hann_window(const std::size_t size) {
  std::vector<float> window(size);
  if (size <= 1U) {
    if (!window.empty()) {
      window.front() = 1.0F;
    }
    return window;
  }
  for (std::size_t index = 0; index < size; ++index) {
    window[index] = 0.5F * (1.0F - std::cos((2.0F * std::numbers::pi_v<float> *
                                              static_cast<float>(index)) /
                                             static_cast<float>(size - 1U)));
  }
  return window;
}

[[nodiscard]] std::size_t find_best_offset(const std::span<const float> reference,
                                           const std::span<const float> candidate,
                                           const std::size_t center) {
  if (candidate.size() < kFrameSize) {
    return 0U;
  }
  const std::size_t max_offset =
      std::min({kSearchRadius, center, candidate.size() - kFrameSize - center});
  std::size_t best_offset = center;
  double best_score = -1.0;
  for (std::size_t offset = center - max_offset; offset <= center + max_offset; ++offset) {
    double score = 0.0;
    for (std::size_t index = 0; index < kFrameSize; ++index) {
      score += static_cast<double>(reference[index]) *
               static_cast<double>(candidate[offset + index]);
    }
    if (score > best_score) {
      best_score = score;
      best_offset = offset;
    }
  }
  return best_offset;
}

void overlap_add(std::span<float> destination, const std::size_t destination_offset,
                 const std::span<const float> grain, const std::span<const float> window) {
  for (std::size_t index = 0; index < kFrameSize; ++index) {
    const std::size_t write_index = destination_offset + index;
    if (write_index >= destination.size()) {
      break;
    }
    destination[write_index] += grain[index] * window[index];
  }
}

void normalize_overlap(std::span<float> buffer, const std::span<const float> window_sum) {
  for (std::size_t index = 0; index < buffer.size(); ++index) {
    const float denominator = window_sum[index];
    if (denominator > 1.0e-6F) {
      buffer[index] /= denominator;
    }
  }
}

void stretch_channel(const std::span<const float> input, std::span<float> output,
                     const std::span<const float> window, std::span<float> overlap,
                     const bool primed) {
  if (input.empty() || output.empty()) {
    return;
  }

  const double speed =
      static_cast<double>(input.size()) / static_cast<double>(output.size());
  const std::size_t analysis_hop =
      std::max<std::size_t>(1U, static_cast<std::size_t>(std::llround(
                                     static_cast<double>(kSynthesisHop) * speed)));

  std::vector<float> window_sum(output.size(), 0.0F);
  std::fill(output.begin(), output.end(), 0.0F);

  std::size_t input_offset = 0U;
  std::size_t output_offset = 0U;
  while (output_offset < output.size()) {
    if (input_offset + kFrameSize > input.size()) {
      break;
    }

    std::vector<float> grain(kFrameSize, 0.0F);
    if (!primed) {
      std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(input_offset), kFrameSize,
                  grain.begin());
    } else {
      const std::size_t best = find_best_offset(overlap, input, input_offset);
      std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(best), kFrameSize, grain.begin());
    }

    overlap_add(output, output_offset, grain, window);
    for (std::size_t index = 0; index < kFrameSize; ++index) {
      const std::size_t write_index = output_offset + index;
      if (write_index < window_sum.size()) {
        window_sum[write_index] += window[index];
      }
    }

    std::copy(grain.begin(), grain.end(), overlap.begin());
    input_offset += analysis_hop;
    output_offset += kSynthesisHop;
  }

  normalize_overlap(output, window_sum);

  for (std::size_t frame = 0; frame < output.size(); ++frame) {
    if (window_sum[frame] > 1.0e-6F) {
      continue;
    }
    const std::size_t mapped = std::min(
        input.size() - 1U,
        static_cast<std::size_t>(std::llround(static_cast<double>(frame) * speed)));
    output[frame] = input[mapped];
  }
}

} // namespace

bool shuttle_pitch_correction_supported(const double transport_rate) noexcept {
  if (transport_rate <= 0.0) {
    return false;
  }
  const double magnitude = transport_rate;
  return std::abs(magnitude - 0.5) < 1.0e-9 || std::abs(magnitude - 2.0) < 1.0e-9 ||
         std::abs(magnitude - 4.0) < 1.0e-9 || std::abs(magnitude - 8.0) < 1.0e-9;
}

void ShuttlePitchCorrector::reset(const double speed_factor,
                                  const std::uint32_t channels) noexcept {
  speed_factor_ = speed_factor;
  channels_ = channels;
  overlap_.assign(channels, std::vector<float>(kFrameSize, 0.0F));
  primed_ = false;
}

void ShuttlePitchCorrector::stretch(const AudioBlock& source, AudioBlock& output) {
  if (source.format().channels != output.format().channels ||
      source.format().sample_rate != output.format().sample_rate) {
    throw std::invalid_argument("shuttle pitch correction requires matching audio formats");
  }
  if (source.format().channels == 0U || output.frame_count() == 0U) {
    output.clear();
    return;
  }
  if (channels_ != source.format().channels ||
      !shuttle_pitch_correction_supported(speed_factor_)) {
    throw std::logic_error("shuttle pitch corrector is not configured for this transport rate");
  }
  if (source.frame_count() == 0U) {
    output.clear();
    return;
  }

  const auto window = hann_window(kFrameSize);
  for (std::uint32_t channel_index = 0; channel_index < source.format().channels; ++channel_index) {
    stretch_channel(source.channel(channel_index), output.channel(channel_index), window,
                    overlap_[channel_index], primed_);
  }
  primed_ = true;
}

} // namespace video_editor::audio
