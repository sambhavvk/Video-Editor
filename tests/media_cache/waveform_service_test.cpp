// SPDX-License-Identifier: MPL-2.0

#include "video_editor/media_cache/waveform_service.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace video_editor::media_cache {
namespace {

// ---------------------------------------------------------------------------
// Helpers for crafting/inspecting serialized blobs.
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::byte> to_bytes(const std::string& text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return out;
}

template <typename Integer>
void append_le(std::vector<std::byte>& out, const Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  const Unsigned bits = static_cast<Unsigned>(value);
  for (std::size_t shift = 0; shift < sizeof(Integer); ++shift) {
    out.push_back(static_cast<std::byte>(
        (bits >> static_cast<Unsigned>(shift * 8U)) & static_cast<Unsigned>(0xFFU)));
  }
}

void append_f32(std::vector<std::byte>& out, const float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  append_le(out, bits);
}

[[nodiscard]] std::vector<std::byte> magic_blob() {
  return to_bytes("VEWAVE01");
}

// Builds a minimal valid blob with the given top-level counts so tests can
// append crafted level/bucket payloads.
[[nodiscard]] std::vector<std::byte> header_blob(const std::int64_t sample_rate,
                                                  const std::int64_t channel_count,
                                                  const std::int64_t total_samples,
                                                  const std::int64_t source_stream_index,
                                                  const std::int32_t level_count) {
  std::vector<std::byte> out = magic_blob();
  append_le(out, sample_rate);
  append_le(out, channel_count);
  append_le(out, total_samples);
  append_le(out, source_stream_index);
  append_le(out, level_count);
  return out;
}

void append_level_header(std::vector<std::byte>& out, const std::int64_t bucket_count,
                         const std::int64_t sample_count, const std::int64_t level_sample_rate,
                         const std::int64_t level_channel_count) {
  append_le(out, bucket_count);
  append_le(out, sample_count);
  append_le(out, level_sample_rate);
  append_le(out, level_channel_count);
}

void append_bucket(std::vector<std::byte>& out, const float minimum, const float maximum,
                   const float rms) {
  append_f32(out, minimum);
  append_f32(out, maximum);
  append_f32(out, rms);
}

// ---------------------------------------------------------------------------
// waveform_level_bucket_counts
// ---------------------------------------------------------------------------

TEST(WaveformLevelBucketCounts, HalveUntilMinimum) {
  const std::vector<std::int64_t> counts = waveform_level_bucket_counts(2000, 8);
  ASSERT_EQ(counts.size(), 8u);
  EXPECT_EQ(counts[0], 2000);
  EXPECT_EQ(counts[1], 1000);
  EXPECT_EQ(counts[2], 500);
  EXPECT_EQ(counts[3], 250);
  EXPECT_EQ(counts[4], 125);
  EXPECT_EQ(counts[5], 62);
  EXPECT_EQ(counts[6], 31);
  EXPECT_EQ(counts[7], 15);
}

TEST(WaveformLevelBucketCounts, StopsBelowTwoBeforeLevelCount) {
  // finest=4, levels=10: {4, 2} then 2/2=1 < 2 -> stop. Only 2 entries.
  const std::vector<std::int64_t> counts = waveform_level_bucket_counts(4, 10);
  ASSERT_EQ(counts.size(), 2u);
  EXPECT_EQ(counts[0], 4);
  EXPECT_EQ(counts[1], 2);
}

TEST(WaveformLevelBucketCounts, EmptyForNonPositiveInput) {
  EXPECT_TRUE(waveform_level_bucket_counts(0, 8).empty());
  EXPECT_TRUE(waveform_level_bucket_counts(2000, 0).empty());
  EXPECT_TRUE(waveform_level_bucket_counts(1, 8).empty());
}

// ---------------------------------------------------------------------------
// waveform_parameter_hash
// ---------------------------------------------------------------------------

TEST(WaveformParameterHash, IsStableAndDistinct) {
  const WaveformOptions a{.finest_level_buckets = 2000,
                          .level_count = 8,
                          .sample_rate = 48000,
                          .channel_count = 1};
  const WaveformOptions b = a;
  EXPECT_EQ(waveform_parameter_hash(a), waveform_parameter_hash(b));
  EXPECT_EQ(waveform_parameter_hash(a), "b2000l8r48000c1");

  WaveformOptions changed_buckets = a;
  changed_buckets.finest_level_buckets = 1000;
  EXPECT_NE(waveform_parameter_hash(changed_buckets), waveform_parameter_hash(a));

  WaveformOptions changed_levels = a;
  changed_levels.level_count = 4;
  EXPECT_NE(waveform_parameter_hash(changed_levels), waveform_parameter_hash(a));

  WaveformOptions changed_rate = a;
  changed_rate.sample_rate = 44100;
  EXPECT_NE(waveform_parameter_hash(changed_rate), waveform_parameter_hash(a));

  WaveformOptions changed_channels = a;
  changed_channels.channel_count = 2;
  EXPECT_NE(waveform_parameter_hash(changed_channels), waveform_parameter_hash(a));
}

// ---------------------------------------------------------------------------
// build_waveform_level
// ---------------------------------------------------------------------------

TEST(BuildWaveformLevel, ComputesMinMaxRmsForSingleBucket) {
  const std::vector<float> samples{0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
  const WaveformLevel level =
      build_waveform_level(samples, 48000, 1, 1);
  ASSERT_EQ(level.buckets.size(), 1u);
  EXPECT_NEAR(level.buckets[0].minimum, -1.0f, 1e-4f);
  EXPECT_NEAR(level.buckets[0].maximum, 1.0f, 1e-4f);
  // rms = sqrt((0 + 0.25 + 0.25 + 1 + 1)/5) = sqrt(0.5) ~= 0.7071
  EXPECT_NEAR(level.buckets[0].rms, 0.70710678f, 1e-4f);
  EXPECT_EQ(level.sample_count, 5);
  EXPECT_EQ(level.bucket_count, 1);
}

TEST(BuildWaveformLevel, MultipleBucketsSplitEvenly) {
  // 6 samples, 3 buckets -> 2 samples each (samples_per_bucket = 6/3 = 2).
  const std::vector<float> samples{0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.25f};
  const WaveformLevel level = build_waveform_level(samples, 48000, 1, 3);
  ASSERT_EQ(level.buckets.size(), 3u);

  // bucket 0: {0.0, 0.5}
  EXPECT_NEAR(level.buckets[0].minimum, 0.0f, 1e-4f);
  EXPECT_NEAR(level.buckets[0].maximum, 0.5f, 1e-4f);
  EXPECT_NEAR(level.buckets[0].rms, std::sqrt((0.0 + 0.25) / 2.0), 1e-4f);

  // bucket 1: {-0.5, 1.0}
  EXPECT_NEAR(level.buckets[1].minimum, -0.5f, 1e-4f);
  EXPECT_NEAR(level.buckets[1].maximum, 1.0f, 1e-4f);
  EXPECT_NEAR(level.buckets[1].rms, std::sqrt((0.25 + 1.0) / 2.0), 1e-4f);

  // bucket 2: {-1.0, 0.25}
  EXPECT_NEAR(level.buckets[2].minimum, -1.0f, 1e-4f);
  EXPECT_NEAR(level.buckets[2].maximum, 0.25f, 1e-4f);
  EXPECT_NEAR(level.buckets[2].rms, std::sqrt((1.0 + 0.0625) / 2.0), 1e-4f);
}

TEST(BuildWaveformLevel, EmptyBucketsUseSentinel) {
  // 2 samples, 4 buckets -> samples_per_bucket = max(1, 2/4) = 1.
  // bucket 0: sample 0; bucket 1: sample 1; buckets 2,3: empty sentinel.
  const std::vector<float> samples{0.25f, -0.75f};
  const WaveformLevel level = build_waveform_level(samples, 48000, 1, 4);
  ASSERT_EQ(level.buckets.size(), 4u);

  EXPECT_NEAR(level.buckets[0].minimum, 0.25f, 1e-4f);
  EXPECT_NEAR(level.buckets[0].maximum, 0.25f, 1e-4f);
  EXPECT_NEAR(level.buckets[0].rms, 0.25f, 1e-4f);

  EXPECT_NEAR(level.buckets[1].minimum, -0.75f, 1e-4f);
  EXPECT_NEAR(level.buckets[1].maximum, -0.75f, 1e-4f);
  EXPECT_NEAR(level.buckets[1].rms, 0.75f, 1e-4f);

  for (std::size_t i = 2; i < 4; ++i) {
    EXPECT_NEAR(level.buckets[i].minimum, 1.0f, 1e-4f);
    EXPECT_NEAR(level.buckets[i].maximum, -1.0f, 1e-4f);
    EXPECT_NEAR(level.buckets[i].rms, 0.0f, 1e-4f);
  }
}

TEST(BuildWaveformLevel, MonoMixdownForStereoInput) {
  // Interleaved stereo {L0,R0,L1,R1} = {0.1, 0.2, 0.3, 0.4}.
  // Mono mixdown = {(0.1+0.2)/2, (0.3+0.4)/2} = {0.15, 0.35}.
  // 1 bucket covers both mono samples.
  const std::vector<float> samples{0.1f, 0.2f, 0.3f, 0.4f};
  const WaveformLevel level = build_waveform_level(samples, 48000, 2, 1);
  ASSERT_EQ(level.buckets.size(), 1u);
  EXPECT_NEAR(level.buckets[0].minimum, 0.15f, 1e-4f);
  EXPECT_NEAR(level.buckets[0].maximum, 0.35f, 1e-4f);
  // rms = sqrt((0.15^2 + 0.35^2)/2)
  const double expected_rms =
      std::sqrt((0.15 * 0.15 + 0.35 * 0.35) / 2.0);
  EXPECT_NEAR(level.buckets[0].rms, static_cast<float>(expected_rms), 1e-4f);
  EXPECT_EQ(level.channel_count, 1);
  EXPECT_EQ(level.sample_count, 2);
}

// ---------------------------------------------------------------------------
// build_waveform_pyramid
// ---------------------------------------------------------------------------

TEST(BuildWaveformPyramid, HasCorrectLevelCountAndBucketCounts) {
  // finest=100, levels=4 -> {100, 50, 25, 12} (25/2 = 12 floor; stop at 4).
  const std::vector<float> samples(1000, 0.1f);
  const WaveformOptions options{.finest_level_buckets = 100,
                                .level_count = 4,
                                .sample_rate = 48000,
                                .channel_count = 1};
  const Waveform waveform = build_waveform_pyramid(samples, 48000, options);
  ASSERT_EQ(waveform.levels.size(), 4u);
  EXPECT_EQ(waveform.levels[0].bucket_count, 100);
  EXPECT_EQ(waveform.levels[1].bucket_count, 50);
  EXPECT_EQ(waveform.levels[2].bucket_count, 25);
  EXPECT_EQ(waveform.levels[3].bucket_count, 12);
  EXPECT_EQ(waveform.total_samples, 1000);
  EXPECT_EQ(waveform.sample_rate, 48000);
  EXPECT_EQ(waveform.channel_count, 1);
}

// ---------------------------------------------------------------------------
// serialize_waveform / deserialize_waveform
// ---------------------------------------------------------------------------

TEST(SerializeDeserialize, RoundTrip) {
  const std::vector<float> samples{0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.25f, -0.25f, 0.0f};
  const WaveformOptions options{.finest_level_buckets = 4,
                                .level_count = 2,
                                .sample_rate = 48000,
                                .channel_count = 1};
  Waveform original = build_waveform_pyramid(samples, 48000, options);
  original.source_stream_index = 1; // ensure non-default is preserved

  const std::vector<std::byte> blob = serialize_waveform(original);
  ASSERT_FALSE(blob.empty());
  const WaveformResult<Waveform> result = deserialize_waveform(blob);
  ASSERT_TRUE(result.has_value()) << result.error().message;

  const Waveform& restored = result.value();
  EXPECT_EQ(restored.sample_rate, original.sample_rate);
  EXPECT_EQ(restored.channel_count, original.channel_count);
  EXPECT_EQ(restored.total_samples, original.total_samples);
  EXPECT_EQ(restored.source_stream_index, original.source_stream_index);
  ASSERT_EQ(restored.levels.size(), original.levels.size());
  for (std::size_t l = 0; l < original.levels.size(); ++l) {
    EXPECT_EQ(restored.levels[l].bucket_count, original.levels[l].bucket_count);
    EXPECT_EQ(restored.levels[l].sample_count, original.levels[l].sample_count);
    EXPECT_EQ(restored.levels[l].sample_rate, original.levels[l].sample_rate);
    EXPECT_EQ(restored.levels[l].channel_count, original.levels[l].channel_count);
    ASSERT_EQ(restored.levels[l].buckets.size(), original.levels[l].buckets.size());
    for (std::size_t b = 0; b < original.levels[l].buckets.size(); ++b) {
      EXPECT_NEAR(restored.levels[l].buckets[b].minimum,
                  original.levels[l].buckets[b].minimum, 1e-6f);
      EXPECT_NEAR(restored.levels[l].buckets[b].maximum,
                  original.levels[l].buckets[b].maximum, 1e-6f);
      EXPECT_NEAR(restored.levels[l].buckets[b].rms,
                  original.levels[l].buckets[b].rms, 1e-6f);
    }
  }
}

TEST(Deserialize, RejectsBadMagic) {
  std::vector<std::byte> blob = to_bytes("BADMAGIC");
  // Pad with zeros so the reader has something to chew on after magic check.
  blob.resize(64, std::byte{0});
  const WaveformResult<Waveform> result = deserialize_waveform(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, WaveformErrorCode::InvalidArgument);
}

TEST(Deserialize, RejectsTrailingBytes) {
  const std::vector<float> samples{0.0f, 0.5f, -0.5f};
  const WaveformOptions options{.finest_level_buckets = 2,
                                .level_count = 1,
                                .sample_rate = 48000,
                                .channel_count = 1};
  const Waveform waveform = build_waveform_pyramid(samples, 48000, options);
  std::vector<std::byte> blob = serialize_waveform(waveform);
  blob.push_back(std::byte{0x42}); // trailing byte
  const WaveformResult<Waveform> result = deserialize_waveform(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, WaveformErrorCode::InvalidArgument);
}

TEST(Deserialize, RejectsNegativeSampleRate) {
  // Header with negative sample_rate, zero levels so no level payload follows.
  std::vector<std::byte> blob = header_blob(-1, 1, 0, -1, 0);
  const WaveformResult<Waveform> result = deserialize_waveform(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, WaveformErrorCode::InvalidArgument);
}

TEST(Deserialize, RejectsInvalidBucket) {
  // One level with one bucket where min > max and not the sentinel.
  std::vector<std::byte> blob = header_blob(48000, 1, 2, 0, 1);
  append_level_header(blob, 1, 2, 48000, 1);
  append_bucket(blob, -0.5f, -0.6f, 0.1f); // min > max, not sentinel
  const WaveformResult<Waveform> result = deserialize_waveform(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, WaveformErrorCode::InvalidArgument);
}

TEST(Deserialize, AcceptsSentinelBucket) {
  // One level with one sentinel bucket (min=1, max=-1, rms=0) is valid.
  std::vector<std::byte> blob = header_blob(48000, 1, 0, 0, 1);
  append_level_header(blob, 1, 0, 48000, 1);
  append_bucket(blob, 1.0f, -1.0f, 0.0f);
  const WaveformResult<Waveform> result = deserialize_waveform(blob);
  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_EQ(result.value().levels.size(), 1u);
  ASSERT_EQ(result.value().levels[0].buckets.size(), 1u);
  EXPECT_NEAR(result.value().levels[0].buckets[0].minimum, 1.0f, 1e-6f);
  EXPECT_NEAR(result.value().levels[0].buckets[0].maximum, -1.0f, 1e-6f);
}

} // namespace
} // namespace video_editor::media_cache
