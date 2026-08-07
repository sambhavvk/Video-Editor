// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_engine/spsc_audio_ring.h"

#include <algorithm>
#include <stdexcept>

namespace video_editor::audio {

SpscAudioRing::SpscAudioRing(const std::size_t capacity_frames, const std::size_t channels)
    : capacity_frames_(capacity_frames), channels_(channels),
      storage_(capacity_frames * channels, 0.0F) {
  if (capacity_frames == 0U || channels == 0U) {
    throw std::invalid_argument("audio ring dimensions must be non-zero");
  }
}

std::size_t SpscAudioRing::available_read_frames() const noexcept {
  const auto write = write_frame_.load(std::memory_order_acquire);
  const auto read = read_frame_.load(std::memory_order_relaxed);
  return static_cast<std::size_t>(write - read);
}

std::size_t SpscAudioRing::available_write_frames() const noexcept {
  return capacity_frames_ - available_read_frames();
}

std::size_t SpscAudioRing::write(const std::span<const float> interleaved) noexcept {
  const std::size_t requested_frames = interleaved.size() / channels_;
  const auto read = read_frame_.load(std::memory_order_acquire);
  const auto write = write_frame_.load(std::memory_order_relaxed);
  const std::size_t writable = std::min<std::size_t>(
      requested_frames, capacity_frames_ - static_cast<std::size_t>(write - read));

  for (std::size_t frame = 0; frame < writable; ++frame) {
    const std::size_t destination_frame = static_cast<std::size_t>((write + frame) % capacity_frames_);
    for (std::size_t channel = 0; channel < channels_; ++channel) {
      storage_[(destination_frame * channels_) + channel] =
          interleaved[(frame * channels_) + channel];
    }
  }
  write_frame_.store(write + writable, std::memory_order_release);
  return writable;
}

std::size_t SpscAudioRing::read(const std::span<float> interleaved) noexcept {
  const std::size_t requested_frames = interleaved.size() / channels_;
  const auto write = write_frame_.load(std::memory_order_acquire);
  const auto read = read_frame_.load(std::memory_order_relaxed);
  const std::size_t readable =
      std::min<std::size_t>(requested_frames, static_cast<std::size_t>(write - read));

  for (std::size_t frame = 0; frame < readable; ++frame) {
    const std::size_t source_frame = static_cast<std::size_t>((read + frame) % capacity_frames_);
    for (std::size_t channel = 0; channel < channels_; ++channel) {
      interleaved[(frame * channels_) + channel] = storage_[(source_frame * channels_) + channel];
    }
  }
  read_frame_.store(read + readable, std::memory_order_release);
  return readable;
}

void SpscAudioRing::reset() noexcept {
  read_frame_.store(0, std::memory_order_relaxed);
  write_frame_.store(0, std::memory_order_relaxed);
}

} // namespace video_editor::audio

