// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/render_engine/frame.h"

#include <array>
#include <cstdint>

namespace video_editor::render {

// Rec.709 preview scope statistics derived from premultiplied RGBA float32
// CpuFrame values. Straight RGB is recovered by unpremultiplying when alpha > 0.
struct ScopeAnalysis final {
  static constexpr int kWaveformColumns = 256;
  static constexpr int kWaveformBins = 256;
  static constexpr int kVectorscopeSize = 128;
  static constexpr int kHistogramBins = 256;

  int source_width{0};
  int source_height{0};
  int waveform_column_count{0};

  // Parade-style waveform: per-column 256-bin luma histograms (Rec.709 Y).
  std::array<std::array<std::uint32_t, kWaveformBins>, kWaveformColumns> waveform{};
  std::array<float, kWaveformColumns> waveform_min{};
  std::array<float, kWaveformColumns> waveform_max{};

  // 2D Cb/Cr histogram centered at (0.5, 0.5). Rec.709 color-bar graticule
  // positions are known constants and are drawn by the UI widget.
  std::array<std::array<std::uint32_t, kVectorscopeSize>, kVectorscopeSize> vectorscope{};

  std::array<std::uint32_t, kHistogramBins> histogram_r{};
  std::array<std::uint32_t, kHistogramBins> histogram_g{};
  std::array<std::uint32_t, kHistogramBins> histogram_b{};
  std::array<std::uint32_t, kHistogramBins> histogram_luma{};
};

class ScopeAnalyzer final {
public:
  [[nodiscard]] static ScopeAnalysis analyze(const CpuFrame& frame);
};

} // namespace video_editor::render
