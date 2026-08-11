// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_cache/cache_store.h"
#include "video_editor/media_codec/types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace video_editor::media_cache {

// A decoded thumbnail image. Bytes are a self-contained JPEG (baseline, no
// exotic markers) small enough for a media-bin grid (default 320px on the long
// edge). The source_pts records the media timestamp the frame was taken from
// so the UI can show "thumbnail at 00:00:01" and so regeneration can be
// deterministic for a given strategy.
struct Thumbnail {
  std::vector<std::byte> jpeg_bytes;
  int width{0};
  int height{0};
  std::int64_t source_pts_microseconds{0};
  std::int64_t source_stream_index{-1};
};

struct ThumbnailOptions {
  // Long-edge target. Default 320. The actual frame is scaled preserving
  // aspect ratio and rounded to even pixels (JPEG requires even dimensions for
  // some chroma subsampling; use YUV420P which needs even dims).
  int maximum_width{320};
  // Strategy for picking the source frame. Middle is the default and is
  // deterministic: floor(duration / 2).
  enum class Strategy : std::uint8_t { First, Middle, Last };
  Strategy strategy{Strategy::Middle};
  // JPEG quality 1-100. Default 85.
  int quality{85};
};

enum class ThumbnailErrorCode : std::uint8_t {
  None,
  NotFound,
  InvalidArgument,
  SourceNotFound,
  NoVideoStream,
  OpenFailed,
  DecodeFailed,
  ScaleFailed,
  EncodeFailed,
  StoreFailed,
  Cancelled,
  Internal,
};

struct ThumbnailError {
  ThumbnailErrorCode code{ThumbnailErrorCode::None};
  int native_code{0};
  std::string message;
};

template <typename T> class ThumbnailResult {
public:
  [[nodiscard]] static ThumbnailResult success(T value) {
    ThumbnailResult r;
    r.value_ = std::move(value);
    return r;
  }
  [[nodiscard]] static ThumbnailResult failure(ThumbnailError error) {
    ThumbnailResult r;
    r.error_ = std::move(error);
    return r;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }
  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_).value(); }
  [[nodiscard]] const ThumbnailError& error() const noexcept { return error_; }

private:
  std::optional<T> value_;
  ThumbnailError error_;
};

// Pure resolver: given a video stream description and options, compute the
// target scaled dimensions (even-aligned) and the source PTS to seek to.
// Exposed for unit testing without FFmpeg.
[[nodiscard]] std::pair<int, int> thumbnail_target_dimensions(const media::VideoDescription& video,
                                                               int maximum_width) noexcept;

// Pure resolver: given stream duration (microseconds, optional) and strategy,
// compute the source PTS in microseconds to seek to. Returns 0 if duration is
// unknown.
[[nodiscard]] std::int64_t thumbnail_source_pts(std::optional<std::int64_t> duration_microseconds,
                                                ThumbnailOptions::Strategy strategy) noexcept;

// Pure: compute the parameter hash for a thumbnail request. Must be stable
// across runs and incorporate maximum_width, strategy, and quality. Use a
// simple hex string of the packed fields (no crypto needed; this is a cache
// key, not a security boundary).
[[nodiscard]] std::string thumbnail_parameter_hash(const ThumbnailOptions& options);

// Generate a thumbnail for one video stream of an asset. The asset_uri must
// exist. The stream_index selects which video stream to use (-1 = best video
// stream). The result is stored in cache_store under
// {asset_id, CacheKind::Thumbnail, thumbnail_parameter_hash(options)} and
// also returned. If a cached entry already exists and is valid, it is
// returned without re-decoding (the store's `contains` check).
[[nodiscard]] ThumbnailResult<Thumbnail>
generate_thumbnail(const std::filesystem::path& asset_uri,
                   int stream_index,
                   const ThumbnailOptions& options,
                   const std::string& asset_id,
                   CacheStore& cache,
                   std::stop_token cancellation = {});

// Load a previously-generated thumbnail from the cache without decoding.
// Returns NotFound (as a ThumbnailError) if absent. The store reference is
// non-const because CacheStore::get updates the last-access time used by LRU
// eviction.
[[nodiscard]] ThumbnailResult<Thumbnail>
load_thumbnail(const std::string& asset_id, const ThumbnailOptions& options, CacheStore& cache);

} // namespace video_editor::media_cache
