// SPDX-License-Identifier: MPL-2.0
#include "video_editor/proxy_service/proxy_service.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace video_editor::proxy {
namespace {

constexpr std::array<std::byte, 8> kPtsMagic{std::byte{'V'}, std::byte{'E'}, std::byte{'P'},
                                             std::byte{'T'}, std::byte{'S'}, std::byte{'M'},
                                             std::byte{'A'}, std::byte{'P'}};
constexpr std::uint64_t kMaximumPtsMapBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr AVRational kMicrosecondsTimeBase{1, AV_TIME_BASE};

class ProxyFailure final : public std::exception {
public:
  explicit ProxyFailure(Error error) : error_(std::move(error)) {}
  [[nodiscard]] const Error& error() const noexcept {
    return error_;
  }

private:
  Error error_;
};

[[noreturn]] void fail(const ErrorCode code, std::string message, const int native_code = 0) {
  throw ProxyFailure({.code = code, .native_code = native_code, .message = std::move(message)});
}

[[nodiscard]] std::string ffmpeg_error(const int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error";
  }
  return std::string(buffer.data());
}

void require_ffmpeg(const int code, const ErrorCode error_code, const std::string_view action) {
  if (code < 0) {
    fail(error_code, std::string(action) + ": " + ffmpeg_error(code), code);
  }
}

struct InputFormatDeleter {
  void operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
      AVFormatContext* local = context;
      avformat_close_input(&local);
    }
  }
};

struct OutputFormatDeleter {
  void operator()(AVFormatContext* context) const noexcept {
    if (context == nullptr) {
      return;
    }
    if (context->pb != nullptr && (context->oformat->flags & AVFMT_NOFILE) == 0) {
      avio_closep(&context->pb);
    }
    avformat_free_context(context);
  }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext* context) const noexcept {
    avcodec_free_context(&context);
  }
};

struct PacketDeleter {
  void operator()(AVPacket* packet) const noexcept {
    av_packet_free(&packet);
  }
};

struct FrameDeleter {
  void operator()(AVFrame* frame) const noexcept {
    av_frame_free(&frame);
  }
};

struct SwsDeleter {
  void operator()(SwsContext* context) const noexcept {
    sws_freeContext(context);
  }
};

struct SwrDeleter {
  void operator()(SwrContext* context) const noexcept {
    swr_free(&context);
  }
};

using InputFormat = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using OutputFormat = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;
using CodecContext = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using ScaleContext = std::unique_ptr<SwsContext, SwsDeleter>;
using ResampleContext = std::unique_ptr<SwrContext, SwrDeleter>;

[[nodiscard]] std::string native_path(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
#else
  return path.string();
#endif
}

[[nodiscard]] int process_id() noexcept {
#ifdef _WIN32
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

[[nodiscard]] std::filesystem::path temporary_sibling(const std::filesystem::path& target,
                                                      const std::string_view purpose) {
  static std::atomic<std::uint64_t> sequence{0};
  const std::uint64_t value = sequence.fetch_add(1, std::memory_order_relaxed);
  return target.parent_path() / (target.filename().string() + "." + std::string(purpose) + "." +
                                 std::to_string(process_id()) + "." + std::to_string(value));
}

class TemporaryFiles final {
public:
  TemporaryFiles(std::filesystem::path proxy, std::filesystem::path map)
      : proxy_(std::move(proxy)), map_(std::move(map)) {}

  TemporaryFiles(const TemporaryFiles&) = delete;
  TemporaryFiles& operator=(const TemporaryFiles&) = delete;

  ~TemporaryFiles() {
    std::error_code ignored;
    std::filesystem::remove(proxy_, ignored);
    std::filesystem::remove(map_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& proxy() const noexcept {
    return proxy_;
  }
  [[nodiscard]] const std::filesystem::path& map() const noexcept {
    return map_;
  }

private:
  std::filesystem::path proxy_;
  std::filesystem::path map_;
};

[[nodiscard]] bool sync_file(const std::filesystem::path& path) noexcept {
  const std::string encoded = native_path(path);
#ifdef _WIN32
  const int descriptor = _open(encoded.c_str(), _O_RDONLY | _O_BINARY);
  if (descriptor < 0) {
    return false;
  }
  const int result = _commit(descriptor);
  _close(descriptor);
#else
  const int descriptor = open(encoded.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  const int result = fsync(descriptor);
  close(descriptor);
#endif
  return result == 0;
}

void sync_directory(const std::filesystem::path& path) noexcept {
#ifndef _WIN32
  const std::string encoded = native_path(path.empty() ? std::filesystem::path(".") : path);
  const int descriptor = open(encoded.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor >= 0) {
    static_cast<void>(fsync(descriptor));
    close(descriptor);
  }
#else
  static_cast<void>(path);
#endif
}

void replace_pair(const std::filesystem::path& proxy_temp,
                  const std::filesystem::path& proxy_destination,
                  const std::filesystem::path& map_temp,
                  const std::filesystem::path& map_destination) {
  const std::filesystem::path proxy_backup = temporary_sibling(proxy_destination, "backup");
  const std::filesystem::path map_backup = temporary_sibling(map_destination, "backup");
  std::error_code error;
  bool proxy_backed_up = false;
  bool map_backed_up = false;
  bool proxy_installed = false;

  if (std::filesystem::exists(proxy_destination, error)) {
    error.clear();
    std::filesystem::rename(proxy_destination, proxy_backup, error);
    if (error) {
      fail(ErrorCode::WriteFailed,
           "cannot preserve the existing proxy before commit: " + error.message(), error.value());
    }
    proxy_backed_up = true;
  }
  error.clear();
  if (std::filesystem::exists(map_destination, error)) {
    error.clear();
    std::filesystem::rename(map_destination, map_backup, error);
    if (error) {
      if (proxy_backed_up) {
        std::error_code rollback;
        std::filesystem::rename(proxy_backup, proxy_destination, rollback);
      }
      fail(ErrorCode::WriteFailed,
           "cannot preserve the existing PTS map before commit: " + error.message(), error.value());
    }
    map_backed_up = true;
  }

  error.clear();
  std::filesystem::rename(proxy_temp, proxy_destination, error);
  if (!error) {
    proxy_installed = true;
    std::filesystem::rename(map_temp, map_destination, error);
  }

  if (error) {
    std::error_code rollback;
    if (proxy_installed) {
      std::filesystem::remove(proxy_destination, rollback);
    }
    if (proxy_backed_up) {
      rollback.clear();
      std::filesystem::rename(proxy_backup, proxy_destination, rollback);
    }
    if (map_backed_up) {
      rollback.clear();
      std::filesystem::rename(map_backup, map_destination, rollback);
    }
    fail(ErrorCode::WriteFailed,
         "cannot atomically commit the proxy and PTS map: " + error.message(), error.value());
  }

  std::error_code ignored;
  if (proxy_backed_up) {
    std::filesystem::remove(proxy_backup, ignored);
  }
  if (map_backed_up) {
    std::filesystem::remove(map_backup, ignored);
  }
  sync_directory(proxy_destination.parent_path());
  if (map_destination.parent_path() != proxy_destination.parent_path()) {
    sync_directory(map_destination.parent_path());
  }
}

void check_cancelled(const std::stop_token cancellation) {
  if (cancellation.stop_requested()) {
    fail(ErrorCode::Cancelled, "proxy generation was cancelled", AVERROR_EXIT);
  }
}

struct InterruptState {
  std::stop_token cancellation;
};

int interrupt_callback(void* opaque) noexcept {
  const auto* state = static_cast<const InterruptState*>(opaque);
  return state != nullptr && state->cancellation.stop_requested() ? 1 : 0;
}

void notify(const ProgressCallback& callback, const Progress& progress) noexcept {
  if (!callback) {
    return;
  }
  try {
    callback(progress);
  } catch (...) {
    // Progress is observational and must never compromise destination safety.
  }
}

[[nodiscard]] const AVCodec* find_prores_encoder(std::string& name) noexcept {
  for (const char* candidate : {"prores_ks", "prores_aw"}) {
    if (const AVCodec* codec = avcodec_find_encoder_by_name(candidate); codec != nullptr) {
      name = candidate;
      return codec;
    }
  }
  if (const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PRORES); codec != nullptr) {
    name = codec->name != nullptr ? codec->name : "prores";
    return codec;
  }
  return nullptr;
}

[[nodiscard]] AVPixelFormat select_pixel_format(const AVCodec* encoder, const VideoCodec codec) {
  const void* values = nullptr;
  int count = 0;
  require_ffmpeg(avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                              &values, &count),
                 ErrorCode::EncoderUnavailable, "query video encoder pixel formats");
  const auto* formats = static_cast<const AVPixelFormat*>(values);
  const auto supported = [formats, count](const AVPixelFormat wanted) {
    if (formats == nullptr) {
      return true;
    }
    return std::find(formats, formats + count, wanted) != formats + count;
  };

  if (supported(AV_PIX_FMT_YUV422P10LE)) {
    return AV_PIX_FMT_YUV422P10LE;
  }
  if (codec == VideoCodec::Ffv1 && supported(AV_PIX_FMT_YUV444P10LE)) {
    return AV_PIX_FMT_YUV444P10LE;
  }
  if (supported(AV_PIX_FMT_YUV422P)) {
    return AV_PIX_FMT_YUV422P;
  }
  if (supported(AV_PIX_FMT_YUV420P)) {
    return AV_PIX_FMT_YUV420P;
  }
  if (formats != nullptr && count > 0) {
    return formats[0];
  }
  fail(ErrorCode::EncoderUnavailable, "video encoder reports no usable pixel format");
}

[[nodiscard]] std::pair<int, int> scaled_dimensions(const int width, const int height,
                                                    const ProxyProfile& profile) {
  const long double requested = static_cast<long double>(profile.scale_numerator) /
                                static_cast<long double>(profile.scale_denominator);
  const long double width_limit =
      static_cast<long double>(profile.maximum_width) / static_cast<long double>(width);
  const long double height_limit =
      static_cast<long double>(profile.maximum_height) / static_cast<long double>(height);
  const long double scale = std::min({requested, width_limit, height_limit});
  auto even = [](const long double value) {
    std::int64_t dimension = static_cast<std::int64_t>(std::floor(value));
    dimension = std::max<std::int64_t>(2, dimension - dimension % 2);
    return static_cast<int>(dimension);
  };
  return {even(static_cast<long double>(width) * scale),
          even(static_cast<long double>(height) * scale)};
}

[[nodiscard]] bool same_resolved_path(const std::filesystem::path& left,
                                      const std::filesystem::path& right) noexcept {
  std::error_code error;
  const std::filesystem::path canonical_left = std::filesystem::weakly_canonical(left, error);
  if (error) {
    return left.lexically_normal() == right.lexically_normal();
  }
  error.clear();
  const std::filesystem::path canonical_right = std::filesystem::weakly_canonical(right, error);
  return !error && canonical_left == canonical_right;
}

void validate_existing_destination(const std::filesystem::path& path,
                                   const std::string_view description) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (error && status.type() != std::filesystem::file_type::not_found) {
    fail(ErrorCode::InvalidArgument,
         "cannot inspect existing " + std::string(description) + ": " + error.message(),
         error.value());
  }
  if (status.type() != std::filesystem::file_type::not_found &&
      status.type() != std::filesystem::file_type::regular) {
    fail(ErrorCode::InvalidArgument,
         "existing " + std::string(description) + " must be a regular file");
  }
}

void copy_stream_metadata(const AVStream* source, AVStream* destination) {
  require_ffmpeg(av_dict_copy(&destination->metadata, source->metadata, 0), ErrorCode::WriteFailed,
                 "copy stream metadata");
  destination->disposition = source->disposition & ~AV_DISPOSITION_ATTACHED_PIC;
  destination->sample_aspect_ratio = source->codecpar->sample_aspect_ratio.num > 0
                                         ? source->codecpar->sample_aspect_ratio
                                         : source->sample_aspect_ratio;

  for (int index = 0; index < source->codecpar->nb_coded_side_data; ++index) {
    const AVPacketSideData& input = source->codecpar->coded_side_data[index];
    AVPacketSideData* output = av_packet_side_data_new(&destination->codecpar->coded_side_data,
                                                       &destination->codecpar->nb_coded_side_data,
                                                       input.type, input.size, 0);
    if (output == nullptr) {
      fail(ErrorCode::WriteFailed, "cannot preserve source stream side data", AVERROR(ENOMEM));
    }
    std::memcpy(output->data, input.data, input.size);
  }
}

[[nodiscard]] std::int64_t media_origin_microseconds(const AVFormatContext* input) {
  if (input->start_time != AV_NOPTS_VALUE) {
    return input->start_time;
  }
  std::int64_t origin = AV_NOPTS_VALUE;
  for (unsigned index = 0; index < input->nb_streams; ++index) {
    const AVStream* stream = input->streams[index];
    if (stream->start_time == AV_NOPTS_VALUE) {
      continue;
    }
    const std::int64_t candidate =
        av_rescale_q(stream->start_time, stream->time_base, kMicrosecondsTimeBase);
    origin = origin == AV_NOPTS_VALUE ? candidate : std::min(origin, candidate);
  }
  return origin == AV_NOPTS_VALUE ? 0 : origin;
}

[[nodiscard]] std::int64_t relative_pts(const std::int64_t source_pts,
                                        const AVRational source_time_base,
                                        const std::int64_t origin_microseconds,
                                        const AVRational output_time_base) {
  const std::int64_t source_origin =
      av_rescale_q(origin_microseconds, kMicrosecondsTimeBase, source_time_base);
  return av_rescale_q(source_pts - source_origin, source_time_base, output_time_base);
}

[[nodiscard]] Frame make_frame() {
  Frame frame(av_frame_alloc());
  if (!frame) {
    fail(ErrorCode::Internal, "cannot allocate an FFmpeg frame", AVERROR(ENOMEM));
  }
  return frame;
}

[[nodiscard]] Packet make_packet() {
  Packet packet(av_packet_alloc());
  if (!packet) {
    fail(ErrorCode::Internal, "cannot allocate an FFmpeg packet", AVERROR(ENOMEM));
  }
  return packet;
}

[[nodiscard]] CodecContext open_decoder(const AVStream* stream) {
  const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (decoder == nullptr) {
    fail(ErrorCode::OpenFailed, "no decoder is available for a selected source stream");
  }
  CodecContext context(avcodec_alloc_context3(decoder));
  if (!context) {
    fail(ErrorCode::Internal, "cannot allocate a decoder context", AVERROR(ENOMEM));
  }
  require_ffmpeg(avcodec_parameters_to_context(context.get(), stream->codecpar),
                 ErrorCode::OpenFailed, "copy decoder parameters");
  context->pkt_timebase = stream->time_base;
  require_ffmpeg(avcodec_open2(context.get(), decoder, nullptr), ErrorCode::OpenFailed,
                 "open source decoder");
  return context;
}

template <typename Integer>
void append_little_endian(std::vector<std::byte>& output, const Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  const Unsigned bits = static_cast<Unsigned>(value);
  for (std::size_t shift = 0; shift < sizeof(Integer); ++shift) {
    output.push_back(static_cast<std::byte>((bits >> (shift * 8U)) & static_cast<Unsigned>(0xFFU)));
  }
}

void append_string(std::vector<std::byte>& output, const std::string& value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    fail(ErrorCode::WriteFailed, "PTS map string exceeds the format limit");
  }
  append_little_endian(output, static_cast<std::uint32_t>(value.size()));
  for (const char byte : value) {
    output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
  }
}

[[nodiscard]] std::vector<std::byte> serialize_pts_map(const PtsMap& map) {
  std::vector<std::byte> output;
  const std::uint64_t frame_count = map.streams.empty() ? 0 : map.streams.front().frames.size();
  const std::uint64_t estimated_size = 128U + frame_count * 32U;
  output.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(estimated_size, std::numeric_limits<std::size_t>::max())));
  output.insert(output.end(), kPtsMagic.begin(), kPtsMagic.end());
  append_little_endian(output, map.version);
  append_little_endian(output, static_cast<std::uint8_t>(map.video_codec));
  append_little_endian(output, static_cast<std::uint8_t>(map.container));
  append_little_endian(output, std::uint16_t{0});
  append_little_endian(output, static_cast<std::uint64_t>(map.source_fingerprint.size));
  append_little_endian(output, map.source_fingerprint.modified_nanoseconds);
  append_string(output, map.source_fingerprint.quick_sha256);
  append_little_endian(output,
                       static_cast<std::uint8_t>(map.source_fingerprint.full_sha256.has_value()));
  if (map.source_fingerprint.full_sha256.has_value()) {
    append_string(output, *map.source_fingerprint.full_sha256);
  }
  if (map.streams.size() > std::numeric_limits<std::uint32_t>::max()) {
    fail(ErrorCode::WriteFailed, "PTS map contains too many streams");
  }
  append_little_endian(output, static_cast<std::uint32_t>(map.streams.size()));
  for (const StreamPtsMap& stream : map.streams) {
    append_little_endian(output, stream.source_stream_index);
    append_little_endian(output, stream.proxy_stream_index);
    append_little_endian(output, stream.source_time_base.numerator);
    append_little_endian(output, stream.source_time_base.denominator);
    append_little_endian(output, stream.proxy_time_base.numerator);
    append_little_endian(output, stream.proxy_time_base.denominator);
    append_little_endian(output, stream.source_origin_pts);
    append_little_endian(output, static_cast<std::uint64_t>(stream.frames.size()));
    for (const FramePtsMapping& frame : stream.frames) {
      append_little_endian(output, frame.source_pts);
      append_little_endian(output, frame.source_duration);
      append_little_endian(output, frame.proxy_pts);
      append_little_endian(output, frame.proxy_duration);
    }
  }
  return output;
}

void write_pts_map(const std::filesystem::path& path, const PtsMap& map) {
  const std::vector<std::byte> bytes = serialize_pts_map(map);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    fail(ErrorCode::WriteFailed, "cannot create the temporary PTS map");
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  if (!output) {
    fail(ErrorCode::WriteFailed, "cannot write the temporary PTS map");
  }
  output.close();
  if (!sync_file(path)) {
    fail(ErrorCode::WriteFailed, "cannot durably sync the temporary PTS map", errno);
  }
}

class ByteReader final {
public:
  explicit ByteReader(const std::span<const std::byte> bytes) : remaining_(bytes) {}

  template <typename Integer> [[nodiscard]] Integer integer() {
    if (remaining_.size() < sizeof(Integer)) {
      fail(ErrorCode::InvalidPtsMap, "PTS map is truncated");
    }
    using Unsigned = std::make_unsigned_t<Integer>;
    std::uintmax_t value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
      value |= static_cast<std::uintmax_t>(std::to_integer<unsigned char>(remaining_[index]))
               << (index * 8U);
    }
    remaining_ = remaining_.subspan(sizeof(Integer));
    return static_cast<Integer>(static_cast<Unsigned>(value));
  }

  [[nodiscard]] std::string string() {
    const std::uint32_t size = integer<std::uint32_t>();
    if (remaining_.size() < size) {
      fail(ErrorCode::InvalidPtsMap, "PTS map string is truncated");
    }
    const auto* characters = reinterpret_cast<const char*>(remaining_.data());
    std::string value(characters, characters + size);
    remaining_ = remaining_.subspan(size);
    return value;
  }

  void magic() {
    if (remaining_.size() < kPtsMagic.size() ||
        !std::equal(kPtsMagic.begin(), kPtsMagic.end(), remaining_.begin())) {
      fail(ErrorCode::InvalidPtsMap, "file is not a Video Editor PTS map");
    }
    remaining_ = remaining_.subspan(kPtsMagic.size());
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return remaining_.size();
  }

private:
  std::span<const std::byte> remaining_;
};

[[nodiscard]] PtsMap parse_pts_map(const std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  reader.magic();
  PtsMap map;
  map.version = reader.integer<std::uint32_t>();
  if (map.version != kPtsMapVersion) {
    fail(ErrorCode::InvalidPtsMap, "unsupported PTS map version");
  }
  map.video_codec = static_cast<VideoCodec>(reader.integer<std::uint8_t>());
  map.container = static_cast<Container>(reader.integer<std::uint8_t>());
  if ((map.video_codec != VideoCodec::ProResProxy && map.video_codec != VideoCodec::Ffv1) ||
      (map.container != Container::QuickTime && map.container != Container::Matroska)) {
    fail(ErrorCode::InvalidPtsMap, "PTS map contains an unknown proxy profile");
  }
  if (reader.integer<std::uint16_t>() != 0) {
    fail(ErrorCode::InvalidPtsMap, "PTS map reserved flags are nonzero");
  }
  map.source_fingerprint.size = reader.integer<std::uint64_t>();
  map.source_fingerprint.modified_nanoseconds = reader.integer<std::int64_t>();
  map.source_fingerprint.quick_sha256 = reader.string();
  const auto valid_sha256 = [](const std::string& value) {
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
             return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
           });
  };
  if (!valid_sha256(map.source_fingerprint.quick_sha256)) {
    fail(ErrorCode::InvalidPtsMap, "PTS map source fingerprint is not a lowercase SHA-256");
  }
  const std::uint8_t has_full_hash = reader.integer<std::uint8_t>();
  if (has_full_hash > 1) {
    fail(ErrorCode::InvalidPtsMap, "PTS map has an invalid full-hash flag");
  }
  if (has_full_hash == 1) {
    map.source_fingerprint.full_sha256 = reader.string();
    if (!valid_sha256(*map.source_fingerprint.full_sha256)) {
      fail(ErrorCode::InvalidPtsMap, "PTS map full fingerprint is not a lowercase SHA-256");
    }
  }
  const std::uint32_t stream_count = reader.integer<std::uint32_t>();
  if (stream_count > reader.remaining() / 40U) {
    fail(ErrorCode::InvalidPtsMap, "PTS map stream count exceeds its payload");
  }
  map.streams.reserve(stream_count);
  for (std::uint32_t stream_index = 0; stream_index < stream_count; ++stream_index) {
    StreamPtsMap stream;
    stream.source_stream_index = reader.integer<std::int32_t>();
    stream.proxy_stream_index = reader.integer<std::int32_t>();
    stream.source_time_base.numerator = reader.integer<std::int32_t>();
    stream.source_time_base.denominator = reader.integer<std::int32_t>();
    stream.proxy_time_base.numerator = reader.integer<std::int32_t>();
    stream.proxy_time_base.denominator = reader.integer<std::int32_t>();
    stream.source_origin_pts = reader.integer<std::int64_t>();
    if (stream.source_time_base.numerator <= 0 || stream.source_time_base.denominator <= 0 ||
        stream.proxy_time_base.numerator <= 0 || stream.proxy_time_base.denominator <= 0) {
      fail(ErrorCode::InvalidPtsMap, "PTS map contains an invalid time base");
    }
    const std::uint64_t frame_count = reader.integer<std::uint64_t>();
    if (frame_count > reader.remaining() / 32U ||
        frame_count > std::numeric_limits<std::size_t>::max()) {
      fail(ErrorCode::InvalidPtsMap, "PTS map frame count exceeds its payload");
    }
    stream.frames.reserve(static_cast<std::size_t>(frame_count));
    for (std::uint64_t frame_index = 0; frame_index < frame_count; ++frame_index) {
      FramePtsMapping frame;
      frame.source_pts = reader.integer<std::int64_t>();
      frame.source_duration = reader.integer<std::int64_t>();
      frame.proxy_pts = reader.integer<std::int64_t>();
      frame.proxy_duration = reader.integer<std::int64_t>();
      if (!stream.frames.empty() && (frame.source_pts < stream.frames.back().source_pts ||
                                     frame.proxy_pts < stream.frames.back().proxy_pts)) {
        fail(ErrorCode::InvalidPtsMap, "PTS map frame timestamps are not monotonic");
      }
      stream.frames.push_back(frame);
    }
    map.streams.push_back(std::move(stream));
  }
  if (reader.remaining() != 0) {
    fail(ErrorCode::InvalidPtsMap, "PTS map has trailing bytes");
  }
  return map;
}

struct VideoPipeline {
  int input_index{-1};
  AVStream* input_stream{nullptr};
  AVStream* output_stream{nullptr};
  CodecContext decoder;
  CodecContext encoder;
  ScaleContext scaler;
  Frame decoded;
  Frame converted;
  std::int64_t synthetic_source_pts{0};
  std::int64_t last_source_pts{AV_NOPTS_VALUE};
};

struct AudioPipeline {
  int input_index{-1};
  AVStream* input_stream{nullptr};
  AVStream* output_stream{nullptr};
  CodecContext decoder;
  CodecContext encoder;
  ResampleContext resampler;
  Frame decoded;
  std::int64_t next_output_pts{AV_NOPTS_VALUE};
};

class Transcoder final {
public:
  Transcoder(const GenerateRequest& request, ResolvedProfile profile,
             assets::FileFingerprint fingerprint, std::stop_token cancellation,
             ProgressCallback progress, const std::filesystem::path& temporary_output)
      : request_(request), profile_(std::move(profile)), fingerprint_(std::move(fingerprint)),
        cancellation_(cancellation), progress_(std::move(progress)),
        temporary_output_(temporary_output), interrupt_{cancellation} {}

  [[nodiscard]] GenerateResult run() {
    open_input();
    configure_output();
    transcode_packets();
    finalize();

    StreamPtsMap stream_map{
        .source_stream_index = video_.input_index,
        .proxy_stream_index = video_.output_stream->index,
        .source_time_base = {video_.input_stream->time_base.num,
                             video_.input_stream->time_base.den},
        .proxy_time_base = {video_.output_stream->time_base.num,
                            video_.output_stream->time_base.den},
        .source_origin_pts = av_rescale_q(origin_microseconds_, kMicrosecondsTimeBase,
                                          video_.input_stream->time_base),
        .frames = std::move(frame_mappings_),
    };
    PtsMap pts_map{
        .version = kPtsMapVersion,
        .video_codec = profile_.video_codec,
        .container = profile_.container,
        .source_fingerprint = fingerprint_,
        .streams = {std::move(stream_map)},
    };
    return {
        .destination = request_.destination,
        .pts_map_path =
            request_.pts_map_destination.value_or(default_pts_map_path(request_.destination)),
        .profile = profile_,
        .pts_map = std::move(pts_map),
        .width = video_.encoder->width,
        .height = video_.encoder->height,
        .audio_included = audio_.has_value(),
        .scanned_source_streams = static_cast<std::uint32_t>(input_->nb_streams),
        .video_frames = video_frames_,
        .audio_samples = audio_samples_,
    };
  }

private:
  void open_input() {
    check_cancelled(cancellation_);
    notify(progress_, {.stage = ProgressStage::Inspecting});
    if (!std::filesystem::is_regular_file(request_.source)) {
      fail(ErrorCode::SourceNotFound, "proxy source does not exist or is not a regular file");
    }

    AVFormatContext* raw_input = avformat_alloc_context();
    if (raw_input == nullptr) {
      fail(ErrorCode::Internal, "cannot allocate an input format context", AVERROR(ENOMEM));
    }
    raw_input->interrupt_callback = {.callback = interrupt_callback, .opaque = &interrupt_};
    const std::string path = native_path(request_.source);
    const int open_result = avformat_open_input(&raw_input, path.c_str(), nullptr, nullptr);
    input_.reset(raw_input);
    if (open_result < 0) {
      check_cancelled(cancellation_);
      require_ffmpeg(open_result, ErrorCode::OpenFailed, "open proxy source");
    }
    const int stream_info_result = avformat_find_stream_info(input_.get(), nullptr);
    if (stream_info_result < 0) {
      check_cancelled(cancellation_);
      require_ffmpeg(stream_info_result, ErrorCode::OpenFailed, "inspect proxy source streams");
    }
    origin_microseconds_ = media_origin_microseconds(input_.get());
    duration_microseconds_ = input_->duration > 0 ? input_->duration : 0;

    const int video_index =
        av_find_best_stream(input_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0) {
      fail(ErrorCode::OpenFailed, "source has no decodable video stream", video_index);
    }
    video_.input_index = video_index;
    video_.input_stream = input_->streams[static_cast<unsigned>(video_index)];
    video_.decoder = open_decoder(video_.input_stream);
    video_.decoded = make_frame();
    video_.converted = make_frame();

    if (request_.profile.include_pcm_audio) {
      const int audio_index =
          av_find_best_stream(input_.get(), AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0);
      if (audio_index >= 0) {
        AudioPipeline pipeline;
        pipeline.input_index = audio_index;
        pipeline.input_stream = input_->streams[static_cast<unsigned>(audio_index)];
        pipeline.decoder = open_decoder(pipeline.input_stream);
        pipeline.decoded = make_frame();
        audio_.emplace(std::move(pipeline));
      }
    }
  }

  void configure_output() {
    check_cancelled(cancellation_);
    const char* muxer = profile_.container == Container::QuickTime ? "mov" : "matroska";
    AVFormatContext* raw_output = nullptr;
    const std::string path = native_path(temporary_output_);
    require_ffmpeg(avformat_alloc_output_context2(&raw_output, nullptr, muxer, path.c_str()),
                   ErrorCode::OpenFailed, "allocate proxy output container");
    output_.reset(raw_output);
    if (!output_) {
      fail(ErrorCode::OpenFailed, "requested proxy container is unavailable");
    }
    output_->flags |= AVFMT_FLAG_BITEXACT;

    configure_video_output();
    if (audio_.has_value()) {
      configure_audio_output();
    }

    if ((output_->oformat->flags & AVFMT_NOFILE) == 0) {
      require_ffmpeg(avio_open(&output_->pb, path.c_str(), AVIO_FLAG_WRITE), ErrorCode::WriteFailed,
                     "open temporary proxy output");
    }
    require_ffmpeg(avformat_write_header(output_.get(), nullptr), ErrorCode::WriteFailed,
                   "write proxy header");

    // Muxers are allowed to select their final time base only at header write.
    map_proxy_time_base_ = video_.output_stream->time_base;
  }

  void configure_video_output() {
    const AVCodec* encoder = avcodec_find_encoder_by_name(profile_.video_encoder.c_str());
    if (encoder == nullptr) {
      fail(ErrorCode::EncoderUnavailable, "resolved video encoder disappeared");
    }
    video_.output_stream = avformat_new_stream(output_.get(), nullptr);
    if (video_.output_stream == nullptr) {
      fail(ErrorCode::Internal, "cannot allocate the proxy video stream", AVERROR(ENOMEM));
    }
    video_.encoder.reset(avcodec_alloc_context3(encoder));
    if (!video_.encoder) {
      fail(ErrorCode::Internal, "cannot allocate the proxy video encoder", AVERROR(ENOMEM));
    }

    const AVCodecContext* decoder = video_.decoder.get();
    AVCodecContext* context = video_.encoder.get();
    if (decoder->width <= 0 || decoder->height <= 0) {
      fail(ErrorCode::OpenFailed, "source video dimensions are invalid");
    }
    const auto [output_width, output_height] =
        scaled_dimensions(decoder->width, decoder->height, request_.profile);
    context->width = output_width;
    context->height = output_height;
    context->pix_fmt = select_pixel_format(encoder, profile_.video_codec);
    context->time_base = video_.input_stream->time_base;
    if (context->time_base.num <= 0 || context->time_base.den <= 0) {
      context->time_base = AVRational{1, 90'000};
    }
    context->framerate = av_guess_frame_rate(input_.get(), video_.input_stream, nullptr);
    const AVCodecParameters* source_parameters = video_.input_stream->codecpar;
    context->sample_aspect_ratio = decoder->sample_aspect_ratio.num > 0
                                       ? decoder->sample_aspect_ratio
                                       : (source_parameters->sample_aspect_ratio.num > 0
                                              ? source_parameters->sample_aspect_ratio
                                              : video_.input_stream->sample_aspect_ratio);
    context->color_primaries = decoder->color_primaries != AVCOL_PRI_UNSPECIFIED
                                   ? decoder->color_primaries
                                   : source_parameters->color_primaries;
    context->color_trc = decoder->color_trc != AVCOL_TRC_UNSPECIFIED ? decoder->color_trc
                                                                     : source_parameters->color_trc;
    context->colorspace = decoder->colorspace != AVCOL_SPC_UNSPECIFIED
                              ? decoder->colorspace
                              : source_parameters->color_space;
    context->color_range = decoder->color_range != AVCOL_RANGE_UNSPECIFIED
                               ? decoder->color_range
                               : source_parameters->color_range;
    context->chroma_sample_location = decoder->chroma_sample_location != AVCHROMA_LOC_UNSPECIFIED
                                          ? decoder->chroma_sample_location
                                          : source_parameters->chroma_location;
    context->field_order = decoder->field_order != AV_FIELD_UNKNOWN
                               ? decoder->field_order
                               : source_parameters->field_order;
    context->flags |= AV_CODEC_FLAG_BITEXACT;
    context->thread_count = 1;
    if ((output_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
      context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (profile_.video_codec == VideoCodec::ProResProxy) {
      context->profile = AV_PROFILE_PRORES_PROXY;
      require_ffmpeg(av_opt_set_int(context->priv_data, "profile", AV_PROFILE_PRORES_PROXY, 0),
                     ErrorCode::EncoderUnavailable, "select ProRes Proxy profile");
    } else {
      // Single-threaded, fixed FFV1 settings avoid machine-dependent choices.
      static_cast<void>(av_opt_set_int(context->priv_data, "level", 3, 0));
      static_cast<void>(av_opt_set_int(context->priv_data, "coder", 1, 0));
      static_cast<void>(av_opt_set_int(context->priv_data, "context", 1, 0));
      static_cast<void>(av_opt_set_int(context->priv_data, "slicecrc", 1, 0));
    }
    require_ffmpeg(avcodec_open2(context, encoder, nullptr), ErrorCode::EncoderUnavailable,
                   "open proxy video encoder");
    require_ffmpeg(avcodec_parameters_from_context(video_.output_stream->codecpar, context),
                   ErrorCode::Internal, "copy proxy video parameters");
    video_.output_stream->codecpar->sample_aspect_ratio = context->sample_aspect_ratio;
    video_.output_stream->codecpar->color_primaries = context->color_primaries;
    video_.output_stream->codecpar->color_trc = context->color_trc;
    video_.output_stream->codecpar->color_space = context->colorspace;
    video_.output_stream->codecpar->color_range = context->color_range;
    video_.output_stream->codecpar->chroma_location = context->chroma_sample_location;
    video_.output_stream->codecpar->field_order = context->field_order;
    if (profile_.video_codec == VideoCodec::ProResProxy) {
      video_.output_stream->codecpar->codec_tag = MKTAG('a', 'p', 'c', 'o');
    }
    video_.output_stream->time_base = context->time_base;
    video_.output_stream->avg_frame_rate = context->framerate;
    copy_stream_metadata(video_.input_stream, video_.output_stream);

    video_.converted->format = context->pix_fmt;
    video_.converted->width = context->width;
    video_.converted->height = context->height;
    require_ffmpeg(av_frame_get_buffer(video_.converted.get(), 32), ErrorCode::Internal,
                   "allocate converted proxy video frame");
    video_.scaler.reset(sws_getContext(
        decoder->width, decoder->height, decoder->pix_fmt, context->width, context->height,
        context->pix_fmt, SWS_BICUBIC | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr));
    if (!video_.scaler) {
      fail(ErrorCode::OpenFailed, "cannot configure proxy video scaling");
    }
  }

  void configure_audio_output() {
    AudioPipeline& audio = *audio_;
    const AVCodec* encoder = avcodec_find_encoder_by_name(profile_.audio_encoder.c_str());
    if (encoder == nullptr) {
      fail(ErrorCode::EncoderUnavailable, "resolved PCM audio encoder disappeared");
    }
    audio.output_stream = avformat_new_stream(output_.get(), nullptr);
    if (audio.output_stream == nullptr) {
      fail(ErrorCode::Internal, "cannot allocate the proxy audio stream", AVERROR(ENOMEM));
    }
    audio.encoder.reset(avcodec_alloc_context3(encoder));
    if (!audio.encoder) {
      fail(ErrorCode::Internal, "cannot allocate the proxy audio encoder", AVERROR(ENOMEM));
    }
    AVCodecContext* context = audio.encoder.get();
    context->sample_rate = 48'000;
    context->sample_fmt = AV_SAMPLE_FMT_S16;
    context->time_base = AVRational{1, context->sample_rate};
    context->bits_per_raw_sample = 16;
    context->flags |= AV_CODEC_FLAG_BITEXACT;
    if ((output_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
      context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    const AVChannelLayout* input_layout = &audio.decoder->ch_layout;
    AVChannelLayout default_layout{};
    if (input_layout->nb_channels <= 0) {
      av_channel_layout_default(&default_layout, 2);
      input_layout = &default_layout;
    } else if (input_layout->order == AV_CHANNEL_ORDER_UNSPEC) {
      av_channel_layout_default(&default_layout, input_layout->nb_channels);
      input_layout = &default_layout;
    }
    require_ffmpeg(av_channel_layout_copy(&context->ch_layout, input_layout), ErrorCode::Internal,
                   "copy proxy audio channel layout");
    require_ffmpeg(avcodec_open2(context, encoder, nullptr), ErrorCode::EncoderUnavailable,
                   "open proxy PCM audio encoder");
    require_ffmpeg(avcodec_parameters_from_context(audio.output_stream->codecpar, context),
                   ErrorCode::Internal, "copy proxy audio parameters");
    audio.output_stream->time_base = context->time_base;
    copy_stream_metadata(audio.input_stream, audio.output_stream);

    SwrContext* raw_resampler = nullptr;
    require_ffmpeg(swr_alloc_set_opts2(&raw_resampler, &context->ch_layout, context->sample_fmt,
                                       context->sample_rate, input_layout,
                                       audio.decoder->sample_fmt, audio.decoder->sample_rate, 0,
                                       nullptr),
                   ErrorCode::OpenFailed, "configure proxy audio resampling");
    audio.resampler.reset(raw_resampler);
    require_ffmpeg(swr_init(audio.resampler.get()), ErrorCode::OpenFailed,
                   "initialize proxy audio resampling");
    av_channel_layout_uninit(&default_layout);
  }

  void transcode_packets() {
    Packet packet = make_packet();
    for (;;) {
      check_cancelled(cancellation_);
      const int result = av_read_frame(input_.get(), packet.get());
      if (result == AVERROR_EOF) {
        break;
      }
      if (result < 0) {
        check_cancelled(cancellation_);
        require_ffmpeg(result, ErrorCode::DecodeFailed, "read source packet");
      }
      if (packet->stream_index == video_.input_index) {
        decode_video_packet(packet.get());
      } else if (audio_.has_value() && packet->stream_index == audio_->input_index) {
        decode_audio_packet(packet.get());
      }
      av_packet_unref(packet.get());
    }
    decode_video_packet(nullptr);
    if (audio_.has_value()) {
      decode_audio_packet(nullptr);
      flush_audio_resampler();
    }
  }

  void decode_video_packet(const AVPacket* packet) {
    require_ffmpeg(avcodec_send_packet(video_.decoder.get(), packet), ErrorCode::DecodeFailed,
                   "send source video packet to decoder");
    for (;;) {
      const int result = avcodec_receive_frame(video_.decoder.get(), video_.decoded.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return;
      }
      require_ffmpeg(result, ErrorCode::DecodeFailed, "decode source video frame");
      process_video_frame();
      av_frame_unref(video_.decoded.get());
    }
  }

  void process_video_frame() {
    check_cancelled(cancellation_);
    AVFrame* source = video_.decoded.get();
    AVFrame* destination = video_.converted.get();
    require_ffmpeg(av_frame_make_writable(destination), ErrorCode::Internal,
                   "make proxy video frame writable");
    const int scaled_rows = sws_scale(video_.scaler.get(), source->data, source->linesize, 0,
                                      source->height, destination->data, destination->linesize);
    if (scaled_rows != destination->height) {
      fail(ErrorCode::EncodeFailed, "video scaler produced an incomplete frame");
    }

    std::int64_t source_pts = source->best_effort_timestamp;
    if (source_pts == AV_NOPTS_VALUE) {
      source_pts = source->pts;
    }
    if (source_pts == AV_NOPTS_VALUE) {
      source_pts = video_.last_source_pts == AV_NOPTS_VALUE
                       ? video_.synthetic_source_pts
                       : video_.last_source_pts + std::max<std::int64_t>(1, source->duration);
    }
    const std::int64_t source_duration = std::max<std::int64_t>(0, source->duration);
    const std::int64_t proxy_pts = relative_pts(source_pts, video_.input_stream->time_base,
                                                origin_microseconds_, video_.encoder->time_base);
    destination->pts = proxy_pts;
    destination->duration =
        av_rescale_q(source_duration, video_.input_stream->time_base, video_.encoder->time_base);
    destination->sample_aspect_ratio = video_.encoder->sample_aspect_ratio;
    destination->color_primaries = source->color_primaries != AVCOL_PRI_UNSPECIFIED
                                       ? source->color_primaries
                                       : video_.encoder->color_primaries;
    destination->color_trc =
        source->color_trc != AVCOL_TRC_UNSPECIFIED ? source->color_trc : video_.encoder->color_trc;
    destination->colorspace = source->colorspace != AVCOL_SPC_UNSPECIFIED
                                  ? source->colorspace
                                  : video_.encoder->colorspace;
    destination->color_range = source->color_range != AVCOL_RANGE_UNSPECIFIED
                                   ? source->color_range
                                   : video_.encoder->color_range;
    destination->chroma_location = source->chroma_location != AVCHROMA_LOC_UNSPECIFIED
                                       ? source->chroma_location
                                       : video_.encoder->chroma_sample_location;
    destination->flags = source->flags & AV_FRAME_FLAG_INTERLACED;

    FramePtsMapping mapping{
        .source_pts = source_pts,
        .source_duration = source_duration,
        .proxy_pts = av_rescale_q(proxy_pts, video_.encoder->time_base, map_proxy_time_base_),
        .proxy_duration =
            av_rescale_q(source_duration, video_.input_stream->time_base, map_proxy_time_base_),
    };
    if (!frame_mappings_.empty() && (mapping.source_pts < frame_mappings_.back().source_pts ||
                                     mapping.proxy_pts < frame_mappings_.back().proxy_pts)) {
      fail(ErrorCode::DecodeFailed,
           "decoder returned non-monotonic presentation timestamps; exact proxy mapping is unsafe");
    }
    frame_mappings_.push_back(mapping);
    video_.last_source_pts = source_pts;
    video_.synthetic_source_pts = source_pts + std::max<std::int64_t>(1, source_duration);
    encode_frame(video_.encoder.get(), video_.output_stream, destination, ErrorCode::EncodeFailed);
    ++video_frames_;
    report_progress(source_pts, video_.input_stream->time_base);
  }

  void decode_audio_packet(const AVPacket* packet) {
    AudioPipeline& audio = *audio_;
    require_ffmpeg(avcodec_send_packet(audio.decoder.get(), packet), ErrorCode::DecodeFailed,
                   "send source audio packet to decoder");
    for (;;) {
      const int result = avcodec_receive_frame(audio.decoder.get(), audio.decoded.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return;
      }
      require_ffmpeg(result, ErrorCode::DecodeFailed, "decode source audio frame");
      process_audio_frame(audio.decoded.get());
      av_frame_unref(audio.decoded.get());
    }
  }

  void process_audio_frame(const AVFrame* source) {
    AudioPipeline& audio = *audio_;
    const int output_capacity = static_cast<int>(av_rescale_rnd(
        swr_get_delay(audio.resampler.get(), audio.decoder->sample_rate) + source->nb_samples,
        audio.encoder->sample_rate, audio.decoder->sample_rate, AV_ROUND_UP));
    if (output_capacity <= 0) {
      return;
    }
    Frame output = make_frame();
    output->format = audio.encoder->sample_fmt;
    output->sample_rate = audio.encoder->sample_rate;
    output->nb_samples = output_capacity;
    require_ffmpeg(av_channel_layout_copy(&output->ch_layout, &audio.encoder->ch_layout),
                   ErrorCode::Internal, "copy converted audio channel layout");
    require_ffmpeg(av_frame_get_buffer(output.get(), 0), ErrorCode::Internal,
                   "allocate converted proxy audio frame");
    const int input_plane_count =
        av_sample_fmt_is_planar(static_cast<AVSampleFormat>(source->format)) != 0
            ? source->ch_layout.nb_channels
            : 1;
    if (input_plane_count <= 0) {
      fail(ErrorCode::DecodeFailed, "decoded audio frame has no channels");
    }
    std::vector<const std::uint8_t*> input_planes(static_cast<std::size_t>(input_plane_count));
    std::copy_n(source->extended_data, input_plane_count, input_planes.begin());
    const int converted = swr_convert(audio.resampler.get(), output->extended_data, output_capacity,
                                      input_planes.data(), source->nb_samples);
    require_ffmpeg(converted, ErrorCode::EncodeFailed, "resample proxy audio");
    if (converted == 0) {
      return;
    }
    output->nb_samples = converted;

    std::int64_t desired_pts = AV_NOPTS_VALUE;
    const std::int64_t source_pts = source->best_effort_timestamp != AV_NOPTS_VALUE
                                        ? source->best_effort_timestamp
                                        : source->pts;
    if (source_pts != AV_NOPTS_VALUE) {
      desired_pts = relative_pts(source_pts, audio.input_stream->time_base, origin_microseconds_,
                                 audio.encoder->time_base);
    }
    if (audio.next_output_pts == AV_NOPTS_VALUE ||
        (desired_pts != AV_NOPTS_VALUE && std::llabs(desired_pts - audio.next_output_pts) > 1)) {
      audio.next_output_pts = desired_pts != AV_NOPTS_VALUE ? desired_pts : 0;
    }
    output->pts = audio.next_output_pts;
    audio.next_output_pts += converted;
    encode_frame(audio.encoder.get(), audio.output_stream, output.get(), ErrorCode::EncodeFailed);
    audio_samples_ += static_cast<std::uint64_t>(converted);
  }

  void flush_audio_resampler() {
    AudioPipeline& audio = *audio_;
    for (;;) {
      const std::int64_t delay = swr_get_delay(audio.resampler.get(), audio.decoder->sample_rate);
      if (delay <= 0) {
        break;
      }
      const int output_capacity = static_cast<int>(av_rescale_rnd(
          delay, audio.encoder->sample_rate, audio.decoder->sample_rate, AV_ROUND_UP));
      if (output_capacity <= 0) {
        break;
      }
      Frame output = make_frame();
      output->format = audio.encoder->sample_fmt;
      output->sample_rate = audio.encoder->sample_rate;
      output->nb_samples = output_capacity;
      require_ffmpeg(av_channel_layout_copy(&output->ch_layout, &audio.encoder->ch_layout),
                     ErrorCode::Internal, "copy flushed audio channel layout");
      require_ffmpeg(av_frame_get_buffer(output.get(), 0), ErrorCode::Internal,
                     "allocate flushed proxy audio frame");
      const int converted =
          swr_convert(audio.resampler.get(), output->extended_data, output_capacity, nullptr, 0);
      require_ffmpeg(converted, ErrorCode::EncodeFailed, "flush proxy audio resampler");
      if (converted == 0) {
        break;
      }
      output->nb_samples = converted;
      output->pts = audio.next_output_pts == AV_NOPTS_VALUE ? 0 : audio.next_output_pts;
      audio.next_output_pts = output->pts + converted;
      encode_frame(audio.encoder.get(), audio.output_stream, output.get(), ErrorCode::EncodeFailed);
      audio_samples_ += static_cast<std::uint64_t>(converted);
    }
  }

  void encode_frame(AVCodecContext* encoder, AVStream* stream, const AVFrame* frame,
                    const ErrorCode code) {
    require_ffmpeg(avcodec_send_frame(encoder, frame), code, "send frame to proxy encoder");
    Packet packet = make_packet();
    for (;;) {
      const int result = avcodec_receive_packet(encoder, packet.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return;
      }
      require_ffmpeg(result, code, "encode proxy frame");
      av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
      packet->stream_index = stream->index;
      packet->pos = -1;
      require_ffmpeg(av_interleaved_write_frame(output_.get(), packet.get()),
                     ErrorCode::WriteFailed, "write proxy packet");
      av_packet_unref(packet.get());
    }
  }

  void finalize() {
    check_cancelled(cancellation_);
    encode_frame(video_.encoder.get(), video_.output_stream, nullptr, ErrorCode::EncodeFailed);
    if (audio_.has_value()) {
      encode_frame(audio_->encoder.get(), audio_->output_stream, nullptr, ErrorCode::EncodeFailed);
    }
    require_ffmpeg(av_write_trailer(output_.get()), ErrorCode::WriteFailed, "write proxy trailer");
    if (output_->pb != nullptr) {
      avio_flush(output_->pb);
      require_ffmpeg(avio_closep(&output_->pb), ErrorCode::WriteFailed, "close proxy output");
    }
    check_cancelled(cancellation_);
  }

  void report_progress(const std::int64_t source_pts, const AVRational time_base) {
    double fraction = 0.0;
    if (duration_microseconds_ > 0) {
      const std::int64_t timestamp = av_rescale_q(source_pts, time_base, kMicrosecondsTimeBase);
      fraction = std::clamp(static_cast<double>(timestamp - origin_microseconds_) /
                                static_cast<double>(duration_microseconds_),
                            0.0, 0.99);
    }
    notify(progress_, {.stage = ProgressStage::Transcoding,
                       .fraction = fraction,
                       .video_frames = video_frames_,
                       .audio_samples = audio_samples_});
  }

  const GenerateRequest& request_;
  ResolvedProfile profile_;
  assets::FileFingerprint fingerprint_;
  std::stop_token cancellation_;
  ProgressCallback progress_;
  std::filesystem::path temporary_output_;
  InterruptState interrupt_;
  InputFormat input_;
  OutputFormat output_;
  VideoPipeline video_;
  std::optional<AudioPipeline> audio_;
  std::int64_t origin_microseconds_{0};
  std::int64_t duration_microseconds_{0};
  AVRational map_proxy_time_base_{1, 1};
  std::vector<FramePtsMapping> frame_mappings_;
  std::uint64_t video_frames_{0};
  std::uint64_t audio_samples_{0};
};

} // namespace

EncoderAvailability encoder_availability() noexcept {
  EncoderAvailability availability;
  std::string prores_name;
  availability.prores_proxy = find_prores_encoder(prores_name) != nullptr;
  availability.prores_encoder = std::move(prores_name);
  if (const AVCodec* ffv1 = avcodec_find_encoder(AV_CODEC_ID_FFV1); ffv1 != nullptr) {
    availability.ffv1 = true;
    availability.ffv1_encoder = ffv1->name != nullptr ? ffv1->name : "ffv1";
  }
  if (const AVCodec* pcm = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE); pcm != nullptr) {
    availability.pcm_s16le = true;
    availability.pcm_encoder = pcm->name != nullptr ? pcm->name : "pcm_s16le";
  }
  return availability;
}

Result<ResolvedProfile> resolve_profile(const ProxyProfile& requested,
                                        const EncoderAvailability& availability) {
  if (requested.video_codec != VideoCodec::ProResProxy &&
      requested.video_codec != VideoCodec::Ffv1) {
    return Result<ResolvedProfile>::failure(
        {.code = ErrorCode::InvalidArgument, .message = "proxy video codec is unknown"});
  }
  if (requested.scale_numerator == 0 || requested.scale_denominator == 0 ||
      requested.maximum_width < 2 || requested.maximum_height < 2) {
    return Result<ResolvedProfile>::failure(
        {.code = ErrorCode::InvalidArgument,
         .message = "proxy scale and maximum dimensions must be positive"});
  }
  if (requested.include_pcm_audio && !availability.pcm_s16le) {
    return Result<ResolvedProfile>::failure(
        {.code = ErrorCode::EncoderUnavailable,
         .message = "the 48 kHz signed PCM encoder is unavailable"});
  }

  ResolvedProfile result{
      .requested = requested,
      .video_codec = VideoCodec::ProResProxy,
      .container = Container::QuickTime,
      .video_encoder = {},
      .audio_encoder = requested.include_pcm_audio ? availability.pcm_encoder : std::string{},
      .used_fallback = false,
  };
  if (requested.video_codec == VideoCodec::ProResProxy && availability.prores_proxy) {
    result.video_codec = VideoCodec::ProResProxy;
    result.container = Container::QuickTime;
    result.video_encoder = availability.prores_encoder;
    return Result<ResolvedProfile>::success(std::move(result));
  }
  const bool fallback_allowed =
      requested.video_codec == VideoCodec::Ffv1 || requested.allow_ffv1_fallback;
  if (fallback_allowed && availability.ffv1) {
    result.video_codec = VideoCodec::Ffv1;
    result.container = Container::Matroska;
    result.video_encoder = availability.ffv1_encoder;
    result.used_fallback = requested.video_codec != VideoCodec::Ffv1;
    return Result<ResolvedProfile>::success(std::move(result));
  }
  return Result<ResolvedProfile>::failure(
      {.code = ErrorCode::EncoderUnavailable,
       .message = requested.video_codec == VideoCodec::ProResProxy
                      ? "ProRes Proxy is unavailable and FFV1 fallback cannot be used"
                      : "FFV1 encoder is unavailable"});
}

ProxyProfile patent_neutral_fallback_profile() noexcept {
  ProxyProfile profile;
  profile.video_codec = VideoCodec::Ffv1;
  profile.allow_ffv1_fallback = false;
  return profile;
}

std::filesystem::path default_pts_map_path(const std::filesystem::path& proxy_path) {
  std::filesystem::path path = proxy_path;
  path += ".vepts";
  return path;
}

Result<GenerateResult> generate_proxy(const GenerateRequest& request,
                                      const std::stop_token cancellation,
                                      ProgressCallback progress) {
  try {
    if (request.source.empty() || request.destination.empty()) {
      fail(ErrorCode::InvalidArgument, "proxy source and destination are required");
    }
    const std::filesystem::path map_path =
        request.pts_map_destination.value_or(default_pts_map_path(request.destination));
    if (request.source == request.destination || request.source == map_path ||
        request.destination == map_path ||
        same_resolved_path(request.source, request.destination) ||
        same_resolved_path(request.source, map_path) ||
        same_resolved_path(request.destination, map_path)) {
      fail(ErrorCode::InvalidArgument, "source, proxy, and PTS map paths must be distinct");
    }
    validate_existing_destination(request.destination, "proxy destination");
    validate_existing_destination(map_path, "PTS map destination");
    check_cancelled(cancellation);

    const Result<ResolvedProfile> resolved =
        resolve_profile(request.profile, encoder_availability());
    if (!resolved) {
      return Result<GenerateResult>::failure(resolved.error());
    }
    const auto fingerprint_result = assets::fingerprint_file(request.source, false);
    if (!fingerprint_result) {
      fail(ErrorCode::SourceNotFound, fingerprint_result.error().message,
           fingerprint_result.error().native_code);
    }

    std::error_code filesystem_error;
    if (!request.destination.parent_path().empty()) {
      std::filesystem::create_directories(request.destination.parent_path(), filesystem_error);
      if (filesystem_error) {
        fail(ErrorCode::WriteFailed,
             "cannot create proxy destination directory: " + filesystem_error.message(),
             filesystem_error.value());
      }
    }
    filesystem_error.clear();
    if (!map_path.parent_path().empty()) {
      std::filesystem::create_directories(map_path.parent_path(), filesystem_error);
      if (filesystem_error) {
        fail(ErrorCode::WriteFailed,
             "cannot create PTS map destination directory: " + filesystem_error.message(),
             filesystem_error.value());
      }
    }

    TemporaryFiles temporary(temporary_sibling(request.destination, "partial"),
                             temporary_sibling(map_path, "partial"));
    Transcoder transcoder(request, resolved.value(), fingerprint_result.value(), cancellation,
                          progress, temporary.proxy());
    GenerateResult result = transcoder.run();
    check_cancelled(cancellation);
    notify(progress, {.stage = ProgressStage::Finalizing,
                      .fraction = 1.0,
                      .video_frames = result.video_frames,
                      .audio_samples = result.audio_samples});
    write_pts_map(temporary.map(), result.pts_map);
    if (!sync_file(temporary.proxy())) {
      fail(ErrorCode::WriteFailed, "cannot durably sync the temporary proxy", errno);
    }
    check_cancelled(cancellation);
    replace_pair(temporary.proxy(), request.destination, temporary.map(), map_path);
    notify(progress, {.stage = ProgressStage::Complete,
                      .fraction = 1.0,
                      .video_frames = result.video_frames,
                      .audio_samples = result.audio_samples});
    return Result<GenerateResult>::success(std::move(result));
  } catch (const ProxyFailure& failure) {
    return Result<GenerateResult>::failure(failure.error());
  } catch (const std::exception& exception) {
    return Result<GenerateResult>::failure(
        {.code = ErrorCode::Internal, .message = exception.what()});
  } catch (...) {
    return Result<GenerateResult>::failure(
        {.code = ErrorCode::Internal, .message = "unknown proxy generation failure"});
  }
}

Result<PtsMap> load_pts_map(const std::filesystem::path& path) {
  try {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
      fail(ErrorCode::OpenFailed, "cannot read PTS map size: " + error.message(), error.value());
    }
    if (size > kMaximumPtsMapBytes || size > std::numeric_limits<std::size_t>::max()) {
      fail(ErrorCode::InvalidPtsMap, "PTS map exceeds the reader safety limit");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      fail(ErrorCode::OpenFailed, "cannot open PTS map");
    }
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) {
      fail(ErrorCode::OpenFailed, "cannot read complete PTS map");
    }
    return Result<PtsMap>::success(parse_pts_map(bytes));
  } catch (const ProxyFailure& failure) {
    return Result<PtsMap>::failure(failure.error());
  } catch (const std::exception& exception) {
    return Result<PtsMap>::failure({.code = ErrorCode::Internal, .message = exception.what()});
  } catch (...) {
    return Result<PtsMap>::failure(
        {.code = ErrorCode::Internal, .message = "unknown PTS map failure"});
  }
}

std::string proxy_parameter_hash(const ProxyProfile& profile) {
  std::string hash;
  hash += "codec=";
  hash += std::to_string(static_cast<unsigned>(profile.video_codec));
  hash += ";scale=";
  hash += std::to_string(profile.scale_numerator);
  hash += "/";
  hash += std::to_string(profile.scale_denominator);
  hash += ";max=";
  hash += std::to_string(profile.maximum_width);
  hash += "x";
  hash += std::to_string(profile.maximum_height);
  hash += ";pcm=";
  hash += profile.include_pcm_audio ? "1" : "0";
  return hash;
}

std::string proxy_parameter_hash(const ResolvedProfile& profile) {
  std::string hash = proxy_parameter_hash(profile.requested);
  hash += ";resolved_codec=";
  hash += std::to_string(static_cast<unsigned>(profile.video_codec));
  hash += ";container=";
  hash += std::to_string(static_cast<unsigned>(profile.container));
  hash += ";fallback=";
  hash += profile.used_fallback ? "1" : "0";
  return hash;
}

namespace {

[[nodiscard]] assets::ProxyCodec proxy_codec_from(const VideoCodec codec) noexcept {
  return codec == VideoCodec::Ffv1 ? assets::ProxyCodec::Ffv1 : assets::ProxyCodec::ProResProxy;
}

[[nodiscard]] std::optional<DiscoveredProxy>
try_complete_proxy(const std::filesystem::path& proxy_path,
                   const std::filesystem::path& pts_map_path,
                   const assets::FileFingerprint& source_fingerprint) {
  if (proxy_path.empty()) {
    return std::nullopt;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(proxy_path, error) || error) {
    return std::nullopt;
  }
  const auto map = load_pts_map(pts_map_path);
  if (!map) {
    return std::nullopt;
  }
  const assets::FileFingerprint& stored = map.value().source_fingerprint;
  const bool hash_matches = stored.quick_sha256 == source_fingerprint.quick_sha256 &&
                            !stored.quick_sha256.empty();
  const bool size_ok = source_fingerprint.size == 0 || stored.size == source_fingerprint.size;
  if (!hash_matches || !size_ok) {
    return std::nullopt;
  }
  return DiscoveredProxy{
      .manifest =
          assets::ProxyManifest{
              .proxy_uri = proxy_path,
              .profile = {.codec = proxy_codec_from(map.value().video_codec),
                          .maximum_width = 1920,
                          .maximum_height = 1080,
                          .include_pcm_audio = true},
              .source_fingerprint = map.value().source_fingerprint,
              .engine_version = "proxy-service-v1",
              .complete = true,
          },
      .pts_map_path = pts_map_path,
  };
}

[[nodiscard]] std::vector<std::string> discovery_parameter_hashes() {
  ProxyProfile hd_prores;
  hd_prores.maximum_width = 1280;
  hd_prores.maximum_height = 720;
  ProxyProfile hd_ffv1 = patent_neutral_fallback_profile();
  hd_ffv1.maximum_width = 1280;
  hd_ffv1.maximum_height = 720;

  std::vector<std::string> hashes{
      proxy_parameter_hash(ProxyProfile{}),
      proxy_parameter_hash(patent_neutral_fallback_profile()),
      proxy_parameter_hash(hd_prores),
      proxy_parameter_hash(hd_ffv1),
  };
  std::sort(hashes.begin(), hashes.end());
  hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
  return hashes;
}

} // namespace

std::optional<DiscoveredProxy>
discover_proxy(const std::string& asset_id, const assets::FileFingerprint& source_fingerprint,
               media_cache::CacheStore& cache,
               const std::optional<std::filesystem::path>& legacy_directory) {
  if (asset_id.empty()) {
    return std::nullopt;
  }

  for (const std::string& hash : discovery_parameter_hashes()) {
    const media_cache::CacheKey proxy_key{.asset_id = asset_id,
                                          .kind = media_cache::CacheKind::Proxy,
                                          .parameter_hash = hash};
    const auto proxy_path = cache.path_for(proxy_key);
    if (!proxy_path) {
      continue;
    }

    const media_cache::CacheKey pts_key{.asset_id = asset_id,
                                        .kind = media_cache::CacheKind::ProxyPtsMap,
                                        .parameter_hash = hash};
    const auto cached_pts = cache.path_for(pts_key);
    const std::filesystem::path pts_path =
        cached_pts ? cached_pts.value() : default_pts_map_path(proxy_path.value());
    if (auto found = try_complete_proxy(proxy_path.value(), pts_path, source_fingerprint)) {
      return found;
    }
  }

  if (!legacy_directory.has_value() || legacy_directory->empty()) {
    return std::nullopt;
  }

  for (const char* suffix : {".proxy.mov", ".proxy.mkv"}) {
    const std::filesystem::path proxy_path = *legacy_directory / (asset_id + suffix);
    const std::filesystem::path pts_path = default_pts_map_path(proxy_path);
    if (auto found = try_complete_proxy(proxy_path, pts_path, source_fingerprint)) {
      return found;
    }
  }
  return std::nullopt;
}

} // namespace video_editor::proxy
