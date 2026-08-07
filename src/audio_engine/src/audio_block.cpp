// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/audio_block.h"

#include <algorithm>
#include <stdexcept>

namespace video_editor::audio {

AudioBlock::AudioBlock(const AudioFormat format, const std::int64_t start_sample,
                       const std::size_t frame_count)
    : format_(format), start_sample_(start_sample), frame_count_(frame_count),
      samples_(frame_count * format.channels, 0.0F) {
  if (format.channels == 0U || format.sample_rate == 0U) {
    throw std::invalid_argument("audio format must have a sample rate and at least one channel");
  }
}

std::span<float> AudioBlock::channel(const std::size_t index) {
  if (index >= format_.channels) {
    throw std::out_of_range("audio channel index is out of range");
  }
  return {samples_.data() + (index * frame_count_), frame_count_};
}

std::span<const float> AudioBlock::channel(const std::size_t index) const {
  if (index >= format_.channels) {
    throw std::out_of_range("audio channel index is out of range");
  }
  return {samples_.data() + (index * frame_count_), frame_count_};
}

std::vector<float> AudioBlock::interleaved() const {
  std::vector<float> result(frame_count_ * format_.channels, 0.0F);
  for (std::size_t frame = 0; frame < frame_count_; ++frame) {
    for (std::size_t channel_index = 0; channel_index < format_.channels; ++channel_index) {
      result[(frame * format_.channels) + channel_index] = channel(channel_index)[frame];
    }
  }
  return result;
}

void AudioBlock::clear() noexcept { std::fill(samples_.begin(), samples_.end(), 0.0F); }

} // namespace video_editor::audio

