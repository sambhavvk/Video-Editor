// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/asset_service/asset_service.h"
#include "video_editor/asset_service/fingerprint.h"
#include "video_editor/media_cache/cache_store.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace video_editor::proxy {

inline constexpr std::uint32_t kPtsMapVersion = 1;

enum class VideoCodec : std::uint8_t { ProResProxy = 1, Ffv1 = 2 };
enum class Container : std::uint8_t { QuickTime = 1, Matroska = 2 };

// The default is the distribution-review profile: half-resolution ProRes Proxy
// in MOV. FFV1/Matroska is the deterministic patent-neutral fallback.
struct ProxyProfile {
  VideoCodec video_codec{VideoCodec::ProResProxy};
  std::uint32_t scale_numerator{1};
  std::uint32_t scale_denominator{2};
  int maximum_width{1920};
  int maximum_height{1080};
  bool include_pcm_audio{true};
  bool allow_ffv1_fallback{true};

  friend bool operator==(const ProxyProfile&, const ProxyProfile&) = default;
};

struct EncoderAvailability {
  bool prores_proxy{false};
  bool ffv1{false};
  bool pcm_s16le{false};
  std::string prores_encoder;
  std::string ffv1_encoder;
  std::string pcm_encoder;
};

struct ResolvedProfile {
  ProxyProfile requested;
  VideoCodec video_codec{VideoCodec::ProResProxy};
  Container container{Container::QuickTime};
  std::string video_encoder;
  std::string audio_encoder;
  bool used_fallback{false};

  friend bool operator==(const ResolvedProfile&, const ResolvedProfile&) = default;
};

enum class ErrorCode : std::uint8_t {
  None,
  InvalidArgument,
  SourceNotFound,
  EncoderUnavailable,
  OpenFailed,
  DecodeFailed,
  EncodeFailed,
  WriteFailed,
  InvalidPtsMap,
  Cancelled,
  Internal,
};

struct Error {
  ErrorCode code{ErrorCode::None};
  int native_code{0};
  std::string message;
};

template <typename T> class Result {
public:
  [[nodiscard]] static Result success(T value) {
    Result result;
    result.value_ = std::move(value);
    return result;
  }

  [[nodiscard]] static Result failure(Error error) {
    Result result;
    result.error_ = std::move(error);
    return result;
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return value_.has_value();
  }
  [[nodiscard]] bool has_value() const noexcept {
    return value_.has_value();
  }
  [[nodiscard]] const T& value() const& {
    return value_.value();
  }
  [[nodiscard]] T&& value() && {
    return std::move(value_).value();
  }
  [[nodiscard]] const Error& error() const noexcept {
    return error_;
  }

private:
  std::optional<T> value_;
  Error error_;
};

struct PtsTimeBase {
  std::int32_t numerator{0};
  std::int32_t denominator{1};

  friend bool operator==(const PtsTimeBase&, const PtsTimeBase&) = default;
};

struct FramePtsMapping {
  std::int64_t source_pts{0};
  std::int64_t source_duration{0};
  std::int64_t proxy_pts{0};
  std::int64_t proxy_duration{0};

  friend bool operator==(const FramePtsMapping&, const FramePtsMapping&) = default;
};

struct StreamPtsMap {
  std::int32_t source_stream_index{-1};
  std::int32_t proxy_stream_index{-1};
  PtsTimeBase source_time_base;
  PtsTimeBase proxy_time_base;
  std::int64_t source_origin_pts{0};
  std::vector<FramePtsMapping> frames;

  friend bool operator==(const StreamPtsMap&, const StreamPtsMap&) = default;
};

// Binary little-endian sidecar contract:
//   "VEPTSMAP", u32 version, u8 codec, u8 container, u16 reserved,
//   u64 source size, i64 source mtime, length-prefixed quick SHA-256, optional
//   length-prefixed full SHA-256, u32 stream count, then stream and frame
//   records composed solely of fixed-width signed/unsigned integers.
// Readers reject unknown versions, trailing bytes, invalid time bases, and
// non-monotonic frame maps. Unknown future versions therefore fail closed.
struct PtsMap {
  std::uint32_t version{kPtsMapVersion};
  VideoCodec video_codec{VideoCodec::ProResProxy};
  Container container{Container::QuickTime};
  assets::FileFingerprint source_fingerprint;
  std::vector<StreamPtsMap> streams;

  friend bool operator==(const PtsMap& left, const PtsMap& right) {
    return left.version == right.version && left.video_codec == right.video_codec &&
           left.container == right.container &&
           left.source_fingerprint.size == right.source_fingerprint.size &&
           left.source_fingerprint.modified_nanoseconds ==
               right.source_fingerprint.modified_nanoseconds &&
           left.source_fingerprint.quick_sha256 == right.source_fingerprint.quick_sha256 &&
           left.source_fingerprint.full_sha256 == right.source_fingerprint.full_sha256 &&
           left.streams == right.streams;
  }
};

enum class ProgressStage : std::uint8_t {
  Inspecting,
  Transcoding,
  Finalizing,
  Complete,
};

struct Progress {
  ProgressStage stage{ProgressStage::Inspecting};
  double fraction{0.0};
  std::uint64_t video_frames{0};
  std::uint64_t audio_samples{0};
};

using ProgressCallback = std::function<void(const Progress&)>;

struct GenerateRequest {
  std::filesystem::path source;
  std::filesystem::path destination;
  std::optional<std::filesystem::path> pts_map_destination;
  ProxyProfile profile;
};

struct GenerateResult {
  std::filesystem::path destination;
  std::filesystem::path pts_map_path;
  ResolvedProfile profile;
  PtsMap pts_map;
  int width{0};
  int height{0};
  bool audio_included{false};
  std::uint32_t scanned_source_streams{0};
  std::uint64_t video_frames{0};
  std::uint64_t audio_samples{0};
};

[[nodiscard]] EncoderAvailability encoder_availability() noexcept;

// Pure resolver exposed so packaging can test the fallback decision without
// depending on the encoders installed on a developer machine.
[[nodiscard]] Result<ResolvedProfile> resolve_profile(const ProxyProfile& requested,
                                                      const EncoderAvailability& availability);

[[nodiscard]] ProxyProfile patent_neutral_fallback_profile() noexcept;
[[nodiscard]] std::filesystem::path default_pts_map_path(const std::filesystem::path& proxy_path);

[[nodiscard]] Result<GenerateResult> generate_proxy(const GenerateRequest& request,
                                                    std::stop_token cancellation = {},
                                                    ProgressCallback progress = {});

[[nodiscard]] Result<PtsMap> load_pts_map(const std::filesystem::path& path);

// Returns the stream map for `source_stream_index`, or the first stream when
// `source_stream_index` is negative and the map is non-empty.
[[nodiscard]] const StreamPtsMap* stream_pts_map(const PtsMap& map,
                                                 int source_stream_index) noexcept;

// Binary search on monotonic `source_pts`. Returns the frame whose half-open
// source interval [source_pts, source_pts + source_duration) contains the
// requested absolute source presentation timestamp.
[[nodiscard]] std::optional<FramePtsMapping>
lookup_frame_by_source_pts(const StreamPtsMap& stream, std::int64_t source_pts) noexcept;

// Binary search on monotonic `proxy_pts`. Returns the frame whose half-open
// proxy interval contains the decoded proxy presentation timestamp.
[[nodiscard]] std::optional<FramePtsMapping>
lookup_frame_by_proxy_pts(const StreamPtsMap& stream, std::int64_t proxy_pts) noexcept;

// Stable cache parameter hash for the default/resolved half-res proxy profile.
[[nodiscard]] std::string proxy_parameter_hash(const ProxyProfile& profile);
[[nodiscard]] std::string proxy_parameter_hash(const ResolvedProfile& profile);

struct DiscoveredProxy {
  assets::ProxyManifest manifest;
  std::filesystem::path pts_map_path;
};

// Discover a complete matching proxy for asset_id whose .vepts source fingerprint
// content_matches `source_fingerprint`. Checks CacheStore Proxy + ProxyPtsMap
// kinds first (using proxy_parameter_hash of default ProxyProfile and of
// patent_neutral_fallback_profile), then optional legacy_directory for
// `{assetId}.proxy.mov` / `{assetId}.proxy.mkv` plus `.vepts` sidecar via
// default_pts_map_path. Ignore incomplete/mismatched/missing files.
[[nodiscard]] std::optional<DiscoveredProxy>
discover_proxy(const std::string& asset_id,
               const assets::FileFingerprint& source_fingerprint,
               media_cache::CacheStore& cache,
               const std::optional<std::filesystem::path>& legacy_directory = std::nullopt);

} // namespace video_editor::proxy
