// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::media {

struct Rational {
  std::int64_t numerator{0};
  std::int64_t denominator{1};

  [[nodiscard]] constexpr bool valid() const noexcept { return denominator != 0; }
};

enum class StreamKind : std::uint8_t {
  Unknown,
  Video,
  Audio,
  Subtitle,
  Data,
  Attachment,
};

struct ColorDescription {
  std::string primaries;
  std::string transfer;
  std::string matrix;
  std::string range;
  std::string chroma_location;
};

struct VideoDescription {
  int width{0};
  int height{0};
  int bit_depth{0};
  std::string pixel_format;
  Rational sample_aspect_ratio{1, 1};
  Rational average_frame_rate;
  Rational real_frame_rate;
  std::int64_t frame_count{0};
  int rotation_degrees{0};
  std::string field_order;
  ColorDescription color;
  bool variable_frame_rate_evidence{false};
  bool has_alpha{false};
};

struct AudioDescription {
  int sample_rate{0};
  int channels{0};
  int bits_per_sample{0};
  std::string sample_format;
  std::string channel_layout;
};

struct StreamDescriptor {
  int index{-1};
  StreamKind kind{StreamKind::Unknown};
  std::string codec_name;
  std::string codec_long_name;
  Rational time_base;
  std::optional<std::int64_t> start_time;
  std::optional<std::int64_t> duration;
  std::int64_t bit_rate{0};
  int disposition{0};
  bool attached_picture{false};
  std::string language;
  std::map<std::string, std::string> metadata;
  std::optional<VideoDescription> video;
  std::optional<AudioDescription> audio;
};

struct AssetDescriptor {
  std::filesystem::path uri;
  std::string format_name;
  std::string format_long_name;
  std::optional<std::int64_t> start_time_microseconds;
  std::optional<std::int64_t> duration_microseconds;
  std::int64_t bit_rate{0};
  int best_video_stream{-1};
  int best_audio_stream{-1};
  std::map<std::string, std::string> metadata;
  std::vector<StreamDescriptor> streams;
};

enum class MediaErrorCode : std::uint8_t {
  None,
  InvalidArgument,
  FileNotFound,
  OpenFailed,
  ProbeFailed,
  Unsupported,
  Cancelled,
  Internal,
};

struct MediaError {
  MediaErrorCode code{MediaErrorCode::None};
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

  [[nodiscard]] static Result failure(MediaError error) {
    Result result;
    result.error_ = std::move(error);
    return result;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }
  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_).value(); }
  [[nodiscard]] const MediaError& error() const noexcept { return error_; }

private:
  std::optional<T> value_;
  MediaError error_;
};

} // namespace video_editor::media

