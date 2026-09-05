// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/scope_analyzer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>

namespace video_editor::render {
namespace {

void set_pixel(CpuFrame& frame, const int x, const int y, const float red, const float green,
               const float blue, const float alpha) {
  auto pixel = frame.pixel(x, y);
  pixel[0] = red * alpha;
  pixel[1] = green * alpha;
  pixel[2] = blue * alpha;
  pixel[3] = alpha;
}

[[nodiscard]] std::uint32_t total_counts(const std::array<std::uint32_t, ScopeAnalysis::kHistogramBins>& bins) {
  return static_cast<std::uint32_t>(
      std::accumulate(bins.begin(), bins.end(), static_cast<std::uint64_t>(0)));
}

[[nodiscard]] std::uint32_t vectorscope_total(const ScopeAnalysis& analysis) {
  std::uint64_t total = 0;
  for (const auto& row : analysis.vectorscope) {
    total += std::accumulate(row.begin(), row.end(), static_cast<std::uint64_t>(0));
  }
  return static_cast<std::uint32_t>(total);
}

} // namespace

TEST(ScopeAnalyzerTest, BlackFrameProducesZeroLumaHistogram) {
  CpuFrame frame(8, 8);
  frame.clear(0.0F, 0.0F, 0.0F, 1.0F);

  const ScopeAnalysis analysis = ScopeAnalyzer::analyze(frame);

  EXPECT_EQ(analysis.source_width, 8);
  EXPECT_EQ(analysis.source_height, 8);
  EXPECT_GT(analysis.waveform_column_count, 0);
  EXPECT_EQ(total_counts(analysis.histogram_luma), 64U);
  EXPECT_GE(analysis.histogram_luma[0], 64U);
  EXPECT_EQ(analysis.histogram_luma[255], 0U);
  for (int column = 0; column < analysis.waveform_column_count; ++column) {
    EXPECT_NEAR(analysis.waveform_min[column], 0.0F, 1.0e-4F);
  }
}

TEST(ScopeAnalyzerTest, WhiteOpaqueFrameProducesHighLumaBins) {
  CpuFrame frame(4, 4);
  frame.clear(1.0F, 1.0F, 1.0F, 1.0F);

  const ScopeAnalysis analysis = ScopeAnalyzer::analyze(frame);

  EXPECT_EQ(total_counts(analysis.histogram_luma), 16U);
  EXPECT_GE(analysis.histogram_luma[255], 16U);
  EXPECT_EQ(analysis.histogram_luma[0], 0U);
  for (int column = 0; column < analysis.waveform_column_count; ++column) {
    EXPECT_NEAR(analysis.waveform_max[column], 1.0F, 1.0e-4F);
  }
}

TEST(ScopeAnalyzerTest, PureRedAndGreenShiftVectorscopeAndRgbHistograms) {
  CpuFrame red_frame(2, 2);
  red_frame.clear(1.0F, 0.0F, 0.0F, 1.0F);
  const ScopeAnalysis red = ScopeAnalyzer::analyze(red_frame);

  CpuFrame green_frame(2, 2);
  green_frame.clear(0.0F, 1.0F, 0.0F, 1.0F);
  const ScopeAnalysis green = ScopeAnalyzer::analyze(green_frame);

  EXPECT_GT(red.histogram_r[255], 0U);
  EXPECT_EQ(red.histogram_g[255], 0U);
  EXPECT_GT(green.histogram_g[255], 0U);
  EXPECT_EQ(green.histogram_r[255], 0U);
  EXPECT_NE(vectorscope_total(red), 0U);
  EXPECT_NE(vectorscope_total(green), 0U);
  EXPECT_NE(red.vectorscope, green.vectorscope);
}

TEST(ScopeAnalyzerTest, PremultipliedHalfAlphaRedUnpremultipliesBeforeAnalysis) {
  CpuFrame frame(1, 1);
  set_pixel(frame, 0, 0, 1.0F, 0.0F, 0.0F, 0.5F);

  const ScopeAnalysis analysis = ScopeAnalyzer::analyze(frame);

  EXPECT_EQ(total_counts(analysis.histogram_r), 1U);
  EXPECT_GE(analysis.histogram_r[255], 1U);
  EXPECT_EQ(analysis.histogram_g[255], 0U);
  EXPECT_EQ(analysis.histogram_b[255], 0U);
}

} // namespace video_editor::render
