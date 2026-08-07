// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace video_editor::audio {

struct AudioFormat {
  std::uint32_t sample_rate{48'000};
  std::uint32_t channels{2};
};

class AudioBlock {
public:
  AudioBlock() = default;
  AudioBlock(AudioFormat format, std::int64_t start_sample, std::size_t frame_count);

  [[nodiscard]] const AudioFormat& format() const noexcept { return format_; }
  [[nodiscard]] std::int64_t start_sample() const noexcept { return start_sample_; }
  [[nodiscard]] std::size_t frame_count() const noexcept { return frame_count_; }
  [[nodiscard]] std::span<float> channel(std::size_t index);
  [[nodiscard]] std::span<const float> channel(std::size_t index) const;
  [[nodiscard]] std::vector<float> interleaved() const;
  void clear() noexcept;

private:
  AudioFormat format_;
  std::int64_t start_sample_{0};
  std::size_t frame_count_{0};
  std::vector<float> samples_;
};

} // namespace video_editor::audio

