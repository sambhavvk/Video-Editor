// SPDX-License-Identifier: MPL-2.0
//
// Unit tests for the pure resolvers in thumbnail_service. FFmpeg-dependent
// generation (open/decode/scale/encode) is exercised by a future integration
// test suite that owns real media fixtures; those tests are intentionally not
// built here to keep unit tests hermetic and fast.

#include "video_editor/media_cache/thumbnail_service.h"

#include <cstdint>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

namespace video_editor::media_cache {
namespace {

using Strategy = ThumbnailOptions::Strategy;

[[nodiscard]] media::VideoDescription make_video(int width, int height) {
  media::VideoDescription v;
  v.width = width;
  v.height = height;
  return v;
}

// ---------------------------------------------------------------------------
// thumbnail_target_dimensions
// ---------------------------------------------------------------------------

TEST(ThumbnailTargetDimensions, ScalesLongEdgeToMaximumAndPreservesAspect) {
  // 1920x1080 landscape, max 320 → 320x180.
  const auto [w1, h1] = thumbnail_target_dimensions(make_video(1920, 1080), 320);
  EXPECT_EQ(w1, 320);
  EXPECT_EQ(h1, 180);

  // 1080x1920 portrait, max 320 → 180x320.
  const auto [w2, h2] = thumbnail_target_dimensions(make_video(1080, 1920), 320);
  EXPECT_EQ(w2, 180);
  EXPECT_EQ(h2, 320);
}

TEST(ThumbnailTargetDimensions, DoesNotUpscaleWhenSourceSmallerThanMaximum) {
  // 800x450, max 1000 → source is smaller than max, no upscale, original dims.
  const auto [w, h] = thumbnail_target_dimensions(make_video(800, 450), 1000);
  EXPECT_EQ(w, 800);
  EXPECT_EQ(h, 450);
}

TEST(ThumbnailTargetDimensions, ScalesDownWhenSourceLargerThanMaximum) {
  // 1280x720, max 1000 → scale down long edge to 1000 → 1000x562.
  const auto [w, h] = thumbnail_target_dimensions(make_video(1280, 720), 1000);
  EXPECT_EQ(w, 1000);
  EXPECT_EQ(h, 562);
}

TEST(ThumbnailTargetDimensions, EvenAlignsOddInput) {
  // 321x181 is odd; even-aligned to 320x180.
  const auto [w, h] = thumbnail_target_dimensions(make_video(321, 181), 1000);
  EXPECT_EQ(w, 320);
  EXPECT_EQ(h, 180);
}

TEST(ThumbnailTargetDimensions, ProducesEvenValuesForOddScaledInputs) {
  // A case where naive rounding would yield odd dims. 1920x1080 → max 321:
  // scale = 321/1920 → width 321 (odd) → even-aligned to 320; height 180.
  const auto [w1, h1] = thumbnail_target_dimensions(make_video(1920, 1080), 321);
  EXPECT_EQ(w1 % 2, 0);
  EXPECT_EQ(h1 % 2, 0);

  // 1001x501, max 100 → long edge 1001 → scale ~0.0999 → 100x50 (both even).
  const auto [w2, h2] = thumbnail_target_dimensions(make_video(1001, 501), 100);
  EXPECT_EQ(w2 % 2, 0);
  EXPECT_EQ(h2 % 2, 0);

  // 999x501, max 100 → long edge 999 → scale ~0.1001 → 100x50.
  const auto [w3, h3] = thumbnail_target_dimensions(make_video(999, 501), 100);
  EXPECT_EQ(w3 % 2, 0);
  EXPECT_EQ(h3 % 2, 0);
}

TEST(ThumbnailTargetDimensions, ReturnsZeroForInvalidDimensions) {
  const auto [w, h] = thumbnail_target_dimensions(make_video(0, 0), 320);
  EXPECT_EQ(w, 0);
  EXPECT_EQ(h, 0);
}

// ---------------------------------------------------------------------------
// thumbnail_source_pts
// ---------------------------------------------------------------------------

TEST(ThumbnailSourcePts, MiddleIsHalfDuration) {
  EXPECT_EQ(thumbnail_source_pts(std::int64_t{1'000'000}, Strategy::Middle), 500'000);
}

TEST(ThumbnailSourcePts, FirstIsZero) {
  EXPECT_EQ(thumbnail_source_pts(std::int64_t{1'000'000}, Strategy::First), 0);
}

TEST(ThumbnailSourcePts, LastIsDurationMinusOne) {
  EXPECT_EQ(thumbnail_source_pts(std::int64_t{1'000'000}, Strategy::Last), 999'999);
}

TEST(ThumbnailSourcePts, UnknownDurationReturnsZero) {
  EXPECT_EQ(thumbnail_source_pts(std::nullopt, Strategy::First), 0);
  EXPECT_EQ(thumbnail_source_pts(std::nullopt, Strategy::Middle), 0);
  EXPECT_EQ(thumbnail_source_pts(std::nullopt, Strategy::Last), 0);
}

TEST(ThumbnailSourcePts, ZeroOrNegativeDurationReturnsZero) {
  EXPECT_EQ(thumbnail_source_pts(std::int64_t{0}, Strategy::Middle), 0);
  EXPECT_EQ(thumbnail_source_pts(std::int64_t{0}, Strategy::Last), 0);
  EXPECT_EQ(thumbnail_source_pts(std::int64_t{-5}, Strategy::Middle), 0);
}

TEST(ThumbnailSourcePts, MiddleFloorsOddDuration) {
  // 1_000_001 / 2 = 500_000 (floor).
  EXPECT_EQ(thumbnail_source_pts(std::int64_t{1'000'001}, Strategy::Middle), 500'000);
}

// ---------------------------------------------------------------------------
// thumbnail_parameter_hash
// ---------------------------------------------------------------------------

TEST(ThumbnailParameterHash, IsStableAndDistinct) {
  ThumbnailOptions a;
  a.maximum_width = 320;
  a.strategy = Strategy::Middle;
  a.quality = 85;

  ThumbnailOptions same = a;
  EXPECT_EQ(thumbnail_parameter_hash(a), thumbnail_parameter_hash(same));

  // Different width → different hash.
  ThumbnailOptions wider = a;
  wider.maximum_width = 640;
  EXPECT_NE(thumbnail_parameter_hash(a), thumbnail_parameter_hash(wider));

  // Different strategy → different hash.
  ThumbnailOptions first = a;
  first.strategy = Strategy::First;
  EXPECT_NE(thumbnail_parameter_hash(a), thumbnail_parameter_hash(first));

  // Different quality → different hash.
  ThumbnailOptions q90 = a;
  q90.quality = 90;
  EXPECT_NE(thumbnail_parameter_hash(a), thumbnail_parameter_hash(q90));
}

TEST(ThumbnailParameterHash, IsDeterministicAcrossCalls) {
  ThumbnailOptions opts;
  opts.maximum_width = 480;
  opts.strategy = Strategy::Last;
  opts.quality = 75;
  const std::string first = thumbnail_parameter_hash(opts);
  const std::string second = thumbnail_parameter_hash(opts);
  EXPECT_EQ(first, second);
}

TEST(ThumbnailParameterHash, EncodesAllFields) {
  // The hash must incorporate width, strategy, and quality. Sanity-check the
  // format so a future refactor doesn't silently drop a field.
  ThumbnailOptions opts;
  opts.maximum_width = 320;
  opts.strategy = Strategy::Middle; // index 1
  opts.quality = 85;
  const std::string hash = thumbnail_parameter_hash(opts);
  EXPECT_NE(hash.find("w320"), std::string::npos);
  EXPECT_NE(hash.find("m1"), std::string::npos);
  EXPECT_NE(hash.find("q85"), std::string::npos);
}

} // namespace
} // namespace video_editor::media_cache
