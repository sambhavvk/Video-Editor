// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace video_editor::audio {

class SpscAudioRing {
public:
  SpscAudioRing(std::size_t capacity_frames, std::size_t channels);

  [[nodiscard]] std::size_t channels() const noexcept { return channels_; }
  [[nodiscard]] std::size_t capacity_frames() const noexcept { return capacity_frames_; }
  [[nodiscard]] std::size_t available_read_frames() const noexcept;
  [[nodiscard]] std::size_t available_write_frames() const noexcept;

  // Input and output are interleaved. These operations allocate no memory and never lock.
  [[nodiscard]] std::size_t write(std::span<const float> interleaved) noexcept;
  [[nodiscard]] std::size_t read(std::span<float> interleaved) noexcept;
  void reset() noexcept;

private:
  std::size_t capacity_frames_;
  std::size_t channels_;
  std::vector<float> storage_;
  alignas(64) std::atomic<std::uint64_t> read_frame_{0};
  alignas(64) std::atomic<std::uint64_t> write_frame_{0};
};

} // namespace video_editor::audio

