// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/scope_analyzer.h"

#include <algorithm>
#include <cmath>

namespace video_editor::render {
namespace {

constexpr int kMaxAnalysisEdge = 512;

[[nodiscard]] float rec709_luma(const float red, const float green, const float blue) noexcept {
  return 0.2126F * red + 0.7152F * green + 0.0722F * blue;
}

[[nodiscard]] float rec709_cb(const float red, const float green, const float blue) noexcept {
  return -0.114572F * red - 0.385428F * green + 0.5F * blue;
}

[[nodiscard]] float rec709_cr(const float red, const float green, const float blue) noexcept {
  return 0.5F * red - 0.454153F * green - 0.045847F * blue;
}

[[nodiscard]] int histogram_bin(const float value) noexcept {
  const float clamped = std::clamp(value, 0.0F, 1.0F);
  const int bin =
      static_cast<int>(clamped * static_cast<float>(ScopeAnalysis::kHistogramBins - 1) + 0.5F);
  return std::clamp(bin, 0, ScopeAnalysis::kHistogramBins - 1);
}

[[nodiscard]] int vectorscope_axis(const float value) noexcept {
  const float clamped = std::clamp(value, 0.0F, 1.0F);
  const int axis = static_cast<int>(
      clamped * static_cast<float>(ScopeAnalysis::kVectorscopeSize - 1) + 0.5F);
  return std::clamp(axis, 0, ScopeAnalysis::kVectorscopeSize - 1);
}

struct StraightPixel final {
  float red{0.0F};
  float green{0.0F};
  float blue{0.0F};
};

[[nodiscard]] StraightPixel unpremultiply(const std::span<const float, 4> pixel) noexcept {
  const float alpha = std::clamp(pixel[3], 0.0F, 1.0F);
  if (alpha <= 0.0F) {
    return {};
  }
  const float inverse = 1.0F / alpha;
  return {.red = std::clamp(pixel[0] * inverse, 0.0F, 1.0F),
          .green = std::clamp(pixel[1] * inverse, 0.0F, 1.0F),
          .blue = std::clamp(pixel[2] * inverse, 0.0F, 1.0F)};
}

void analyze_pixels(const int width, const int height, const CpuFrame& frame,
                    const int sample_step_x, const int sample_step_y, ScopeAnalysis& analysis) {
  const int column_count =
      std::min(width, ScopeAnalysis::kWaveformColumns);
  const int column_stride = std::max(1, width / std::max(1, column_count));
  analysis.waveform_column_count = column_count;
  for (int column = 0; column < column_count; ++column) {
    analysis.waveform_min[column] = 1.0F;
    analysis.waveform_max[column] = 0.0F;
  }

  for (int y = 0; y < height; y += sample_step_y) {
    for (int x = 0; x < width; x += sample_step_x) {
      const StraightPixel rgb = unpremultiply(frame.pixel(x, y));
      const float luma = rec709_luma(rgb.red, rgb.green, rgb.blue);
      const float cb = rec709_cb(rgb.red, rgb.green, rgb.blue);
      const float cr = rec709_cr(rgb.red, rgb.green, rgb.blue);

      ++analysis.histogram_r[histogram_bin(rgb.red)];
      ++analysis.histogram_g[histogram_bin(rgb.green)];
      ++analysis.histogram_b[histogram_bin(rgb.blue)];
      ++analysis.histogram_luma[histogram_bin(luma)];

      const int column = std::min(column_count - 1, x / column_stride);
      const int luma_bin = histogram_bin(luma);
      ++analysis.waveform[column][luma_bin];
      analysis.waveform_min[column] = std::min(analysis.waveform_min[column], luma);
      analysis.waveform_max[column] = std::max(analysis.waveform_max[column], luma);

      ++analysis.vectorscope[vectorscope_axis(cb)][vectorscope_axis(cr)];
    }
  }
}

} // namespace

ScopeAnalysis ScopeAnalyzer::analyze(const CpuFrame& frame) {
  ScopeAnalysis analysis{};
  analysis.source_width = frame.width();
  analysis.source_height = frame.height();
  if (frame.width() <= 0 || frame.height() <= 0) {
    return analysis;
  }

  const int long_edge = std::max(frame.width(), frame.height());
  if (long_edge <= kMaxAnalysisEdge) {
    analyze_pixels(frame.width(), frame.height(), frame, 1, 1, analysis);
    return analysis;
  }

  const double scale = static_cast<double>(kMaxAnalysisEdge) / static_cast<double>(long_edge);
  const int sample_width = std::max(1, static_cast<int>(std::lround(frame.width() * scale)));
  const int sample_height = std::max(1, static_cast<int>(std::lround(frame.height() * scale)));
  const int step_x = std::max(1, frame.width() / sample_width);
  const int step_y = std::max(1, frame.height() / sample_height);
  analyze_pixels(frame.width(), frame.height(), frame, step_x, step_y, analysis);
  return analysis;
}

} // namespace video_editor::render
