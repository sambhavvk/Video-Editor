// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_cache/cache_store.h"
#include "video_editor/media_codec/types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace video_editor::media_cache {

// A waveform pyramid. Each level halves the resolution. Level 0 is the
// finest (most buckets). The pyramid lets the UI draw a waveform at any zoom
// without re-decoding. Each bucket holds min, max, and RMS of the samples it
// represents, in normalized float [-1, 1].
struct WaveformBucket {
  float minimum{-1.0f};
  float maximum{1.0f};
  float rms{0.0f};
};

struct WaveformLevel {
  std::int64_t sample_rate{48000};
  std::int64_t channel_count{1};
  std::int64_t sample_count{0};      // total samples at this level (before bucketing)
  std::int64_t bucket_count{0};
  std::vector<WaveformBucket> buckets;
};

struct Waveform {
  std::int64_t source_stream_index{-1};
  std::int64_t sample_rate{48000};
  std::int64_t channel_count{1};
  std::int64_t total_samples{0};
  std::vector<WaveformLevel> levels;  // levels[0] finest
};

struct WaveformOptions {
  // Number of buckets at the finest level. Default 2000. Each coarser level
  // halves this until fewer than 2 buckets remain.
  std::int64_t finest_level_buckets{2000};
  // Number of pyramid levels. Default 8.
  int level_count{8};
  // Decode target sample rate. Default 48000.
  std::int64_t sample_rate{48000};
  // Decode target channel count. Default 1 (mono mixdown — waveforms do not
  // need stereo).
  std::int64_t channel_count{1};
};

enum class WaveformErrorCode : std::uint8_t {
  None,
  NotFound,
  InvalidArgument,
  SourceNotFound,
  NoAudioStream,
  OpenFailed,
  DecodeFailed,
  ResampleFailed,
  StoreFailed,
  Cancelled,
  Internal,
};

struct WaveformError {
  WaveformErrorCode code{WaveformErrorCode::None};
  int native_code{0};
  std::string message;
};

// Mirrors CacheResult in cache_store.h: success carries a value, failure
// carries an error. Use `static success`/`static failure`, `has_value`,
// `value`, and `error`.
template <typename T> class WaveformResult {
public:
  [[nodiscard]] static WaveformResult success(T value) {
    WaveformResult result;
    result.value_ = std::move(value);
    return result;
  }
  [[nodiscard]] static WaveformResult failure(WaveformError error) {
    WaveformResult result;
    result.error_ = std::move(error);
    return result;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }
  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_).value(); }
  [[nodiscard]] const WaveformError& error() const noexcept { return error_; }

private:
  std::optional<T> value_;
  WaveformError error_;
};

// Pure: compute the bucket count at each pyramid level given the finest-level
// bucket count and level count. Halve each level; stop when fewer than 2
// buckets. Returns at most level_count entries (or fewer if halving drops
// below 2 before reaching level_count).
[[nodiscard]] std::vector<std::int64_t>
waveform_level_bucket_counts(std::int64_t finest_level_buckets, int level_count) noexcept;

// Pure: compute the parameter hash for a waveform request. Incorporate
// finest_level_buckets, level_count, sample_rate, channel_count.
[[nodiscard]] std::string waveform_parameter_hash(const WaveformOptions& options);

// Pure: downsample a sequence of normalized float samples into a single
// level of buckets. Each bucket covers `samples_per_bucket` consecutive
// samples (last bucket may be partial). min = min(samples), max = max(samples),
// rms = sqrt(mean(samples^2)). Empty buckets (no samples) get min=1, max=-1,
// rms=0 (sentinel "no data" — min>max signals silence/empty). Exposed for
// unit testing without FFmpeg. Multi-channel interleaved input is mixed down
// to mono before bucketing.
[[nodiscard]] WaveformLevel
build_waveform_level(const std::vector<float>& samples,
                     std::int64_t sample_rate,
                     std::int64_t channel_count,
                     std::int64_t bucket_count);

// Pure: build a full pyramid from a flat mono sample buffer by repeatedly
// calling build_waveform_level with halved bucket counts.
[[nodiscard]] Waveform
build_waveform_pyramid(const std::vector<float>& mono_samples,
                       std::int64_t sample_rate,
                       const WaveformOptions& options);

// Serialize/deserialize a Waveform to/from a compact little-endian binary
// blob for cache storage. Magic: "VEWAVE01". Layout:
//   magic(8), i64 sample_rate, i64 channel_count, i64 total_samples,
//   i64 source_stream_index, i32 level_count, then per level:
//     i64 bucket_count, i64 sample_count, i64 level_sample_rate, i64 level_channel_count,
//     then bucket_count * (f32 min, f32 max, f32 rms)
// Readers reject unknown magic, truncated data, negative counts, and
// min>max buckets (sentinel is allowed only when both are the sentinel values).
[[nodiscard]] std::vector<std::byte> serialize_waveform(const Waveform& waveform);
[[nodiscard]] WaveformResult<Waveform> deserialize_waveform(std::span<const std::byte> bytes);

// Generate a waveform for one audio stream. stream_index -1 = best audio.
// Result is cached and returned. If cache contains a valid entry, it is
// returned without re-decoding.
[[nodiscard]] WaveformResult<Waveform>
generate_waveform(const std::filesystem::path& asset_uri,
                  int stream_index,
                  const WaveformOptions& options,
                  const std::string& asset_id,
                  CacheStore& cache,
                  std::stop_token cancellation = {});

[[nodiscard]] WaveformResult<Waveform>
load_waveform(const std::string& asset_id, const WaveformOptions& options, CacheStore& cache);

} // namespace video_editor::media_cache
