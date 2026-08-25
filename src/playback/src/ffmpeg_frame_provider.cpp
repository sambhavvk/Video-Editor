// SPDX-License-Identifier: MPL-2.0
#include "video_editor/playback/ffmpeg_frame_provider.h"
#include "video_editor/media_codec/format_open.h"
#include "video_editor/proxy_service/proxy_service.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace video_editor::playback {
namespace {

struct FormatContextCloser final {
  void operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
      avformat_close_input(&context);
    }
  }
};

struct CodecContextCloser final {
  void operator()(AVCodecContext* context) const noexcept {
    avcodec_free_context(&context);
  }
};

struct FrameCloser final {
  void operator()(AVFrame* frame) const noexcept {
    av_frame_free(&frame);
  }
};

struct PacketCloser final {
  void operator()(AVPacket* packet) const noexcept {
    av_packet_free(&packet);
  }
};

struct SwsContextCloser final {
  void operator()(SwsContext* context) const noexcept {
    sws_freeContext(context);
  }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextCloser>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextCloser>;

constexpr std::int64_t kMaximumSequentialGapSeconds = 2;
constexpr int kMaximumOutputDimension = 32'768;

[[nodiscard]] std::string ffmpeg_error_string(const int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(error, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error";
  }
  return buffer.data();
}

[[nodiscard]] std::string ffmpeg_message(std::string message, const int error) {
  if (error < 0) {
    message += ": " + ffmpeg_error_string(error);
  }
  return message;
}

template <typename T>
[[nodiscard]] render::RenderResult<T> failure(const render::RenderErrorCode code,
                                              std::string message) {
  return render::RenderResult<T>::failure({.code = code, .message = std::move(message)});
}

[[nodiscard]] bool same_source(const ResolvedAssetStream& left, const ResolvedAssetStream& right) {
  return left.location == right.location && left.is_proxy == right.is_proxy &&
         left.registry_generation == right.registry_generation && left.pts_map == right.pts_map;
}

[[nodiscard]] AVRational pts_time_base(const proxy::PtsTimeBase& time_base) {
  return AVRational{time_base.numerator, time_base.denominator};
}

[[nodiscard]] std::optional<edit::Time> time_from_stream_ticks(const std::int64_t ticks,
                                                               const AVRational time_base) {
  if (time_base.num <= 0 || time_base.den <= 0) {
    return std::nullopt;
  }
  const auto value = static_cast<__int128_t>(ticks) * static_cast<__int128_t>(time_base.num);
  if (value < static_cast<__int128_t>(std::numeric_limits<std::int64_t>::min()) ||
      value > static_cast<__int128_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  try {
    return edit::Time(static_cast<std::int64_t>(value), static_cast<std::uint32_t>(time_base.den))
        .normalized();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::int64_t> stream_ticks_for_time(const edit::Time time,
                                                                const AVRational time_base) {
  if (time.isNegative() || time_base.num <= 0 || time_base.den <= 0) {
    return std::nullopt;
  }
  const auto numerator =
      static_cast<__int128_t>(time.value()) * static_cast<__int128_t>(time_base.den);
  const auto denominator =
      static_cast<__int128_t>(time.timescale()) * static_cast<__int128_t>(time_base.num);
  const auto ticks = numerator / denominator;
  if (ticks < static_cast<__int128_t>(std::numeric_limits<std::int64_t>::min()) ||
      ticks > static_cast<__int128_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(ticks);
}

[[nodiscard]] bool add_would_overflow(const std::int64_t left, const std::int64_t right) {
  return (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
         (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right);
}

[[nodiscard]] bool subtract_would_overflow(const std::int64_t left, const std::int64_t right) {
  return (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
         (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right);
}

struct InterruptState final {
  const std::atomic<std::uint64_t>* current_epoch{nullptr};
  std::atomic<std::uint64_t> request_epoch{0};
};

int interrupt_callback(void* opaque) {
  const auto* state = static_cast<const InterruptState*>(opaque);
  if (state == nullptr || state->current_epoch == nullptr) {
    return 0;
  }
  return state->current_epoch->load(std::memory_order_acquire) !=
                 state->request_epoch.load(std::memory_order_relaxed)
             ? 1
             : 0;
}

struct StoredFrame final {
  FramePtr frame;
  edit::TimeRange presentation;
};

struct RawFrame final {
  FramePtr frame;
  std::int64_t timestamp{AV_NOPTS_VALUE};
  std::int64_t duration_ticks{0};
};

struct DecodeSession final {
  // Must outlive format because AVFormatContext stores a pointer to this state.
  InterruptState interrupt;
  ResolvedAssetStream source;
  FormatContextPtr format;
  CodecContextPtr decoder;
  PacketPtr packet;
  FramePtr scratch_frame;
  SwsContextPtr scaler;
  AVStream* stream{nullptr};
  int stream_index{-1};
  std::int64_t origin_pts{0};
  const proxy::StreamPtsMap* mapped_stream{nullptr};
  bool draining{false};
  std::optional<RawFrame> queued_frame;
  std::optional<StoredFrame> current_frame;
};

enum class ReadStatus : std::uint8_t { Frame, EndOfStream, Stale, Failure };

struct ReadResult final {
  ReadStatus status{ReadStatus::Failure};
  int native_code{0};
  std::string message;
};

struct FrameReadResult final {
  ReadStatus status{ReadStatus::Failure};
  RawFrame frame;
  int native_code{0};
  std::string message;
};

[[nodiscard]] bool stale(const DecodeSession& session) {
  return session.interrupt.current_epoch != nullptr &&
         session.interrupt.current_epoch->load(std::memory_order_acquire) !=
             session.interrupt.request_epoch.load(std::memory_order_relaxed);
}

[[nodiscard]] ReadResult receive_next_frame(DecodeSession& session,
                                            PlaybackStatistics& statistics) {
  for (;;) {
    if (stale(session)) {
      return {.status = ReadStatus::Stale,
              .native_code = AVERROR_EXIT,
              .message = "playback request belongs to a stale epoch"};
    }

    const int receive_result =
        avcodec_receive_frame(session.decoder.get(), session.scratch_frame.get());
    if (receive_result == 0) {
      ++statistics.decoded_frames;
      return {.status = ReadStatus::Frame, .native_code = 0, .message = {}};
    }
    if (receive_result == AVERROR_EOF) {
      return {.status = ReadStatus::EndOfStream, .native_code = 0, .message = {}};
    }
    if (receive_result != AVERROR(EAGAIN)) {
      return {.status = stale(session) ? ReadStatus::Stale : ReadStatus::Failure,
              .native_code = receive_result,
              .message = "cannot receive a decoded video frame"};
    }
    if (session.draining) {
      return {.status = ReadStatus::EndOfStream, .native_code = 0, .message = {}};
    }

    int read_result = 0;
    do {
      av_packet_unref(session.packet.get());
      read_result = av_read_frame(session.format.get(), session.packet.get());
      if (read_result < 0) {
        break;
      }
    } while (session.packet->stream_index != session.stream_index);

    if (read_result == AVERROR_EOF) {
      session.draining = true;
      const int drain_result = avcodec_send_packet(session.decoder.get(), nullptr);
      if (drain_result < 0 && drain_result != AVERROR_EOF) {
        return {.status = ReadStatus::Failure,
                .native_code = drain_result,
                .message = "cannot drain the video decoder"};
      }
      continue;
    }
    if (read_result < 0) {
      return {.status = stale(session) ? ReadStatus::Stale : ReadStatus::Failure,
              .native_code = read_result,
              .message = "cannot read the next media packet"};
    }

    const int send_result = avcodec_send_packet(session.decoder.get(), session.packet.get());
    av_packet_unref(session.packet.get());
    if (send_result < 0) {
      return {.status = stale(session) ? ReadStatus::Stale : ReadStatus::Failure,
              .native_code = send_result,
              .message = "cannot submit a video packet to the decoder"};
    }
  }
}

[[nodiscard]] FrameReadResult take_next_frame(DecodeSession& session,
                                              PlaybackStatistics& statistics) {
  if (session.queued_frame.has_value()) {
    RawFrame frame = std::move(*session.queued_frame);
    session.queued_frame.reset();
    return {
        .status = ReadStatus::Frame, .frame = std::move(frame), .native_code = 0, .message = {}};
  }

  const ReadResult read = receive_next_frame(session, statistics);
  if (read.status != ReadStatus::Frame) {
    return {.status = read.status,
            .frame = {},
            .native_code = read.native_code,
            .message = read.message};
  }

  std::int64_t timestamp = session.scratch_frame->best_effort_timestamp;
  if (timestamp == AV_NOPTS_VALUE) {
    timestamp = session.scratch_frame->pts;
  }
  if (timestamp == AV_NOPTS_VALUE) {
    return {.status = ReadStatus::Failure,
            .frame = {},
            .native_code = 0,
            .message = "decoded video frame has no presentation timestamp"};
  }

  FramePtr owned(av_frame_alloc());
  if (!owned) {
    return {.status = ReadStatus::Failure,
            .frame = {},
            .native_code = AVERROR(ENOMEM),
            .message = "cannot retain the decoded video frame"};
  }
  av_frame_move_ref(owned.get(), session.scratch_frame.get());
  const std::int64_t duration_ticks = owned->duration;
  return {.status = ReadStatus::Frame,
          .frame = {.frame = std::move(owned),
                    .timestamp = timestamp,
                    .duration_ticks = duration_ticks},
          .native_code = 0,
          .message = {}};
}

[[nodiscard]] std::optional<edit::TimeRange>
presentation_for_frame(const DecodeSession& session, const std::int64_t timestamp,
                       const std::int64_t duration_ticks) {
  if (duration_ticks <= 0 || subtract_would_overflow(timestamp, session.origin_pts)) {
    return std::nullopt;
  }
  const std::int64_t relative_timestamp = timestamp - session.origin_pts;
  const auto start = time_from_stream_ticks(relative_timestamp, session.stream->time_base);
  const auto duration = time_from_stream_ticks(duration_ticks, session.stream->time_base);
  if (!start.has_value() || !duration.has_value() || duration->isNegative() || duration->isZero()) {
    return std::nullopt;
  }
  try {
    return edit::TimeRange(*start, *duration);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<edit::TimeRange>
presentation_for_mapping(const proxy::StreamPtsMap& stream,
                         const proxy::FramePtsMapping& mapping) {
  if (mapping.source_duration <= 0 ||
      subtract_would_overflow(mapping.source_pts, stream.source_origin_pts)) {
    return std::nullopt;
  }
  const std::int64_t relative_timestamp = mapping.source_pts - stream.source_origin_pts;
  const AVRational source_time_base = pts_time_base(stream.source_time_base);
  const auto start = time_from_stream_ticks(relative_timestamp, source_time_base);
  const auto duration = time_from_stream_ticks(mapping.source_duration, source_time_base);
  if (!start.has_value() || !duration.has_value() || duration->isNegative() || duration->isZero()) {
    return std::nullopt;
  }
  try {
    return edit::TimeRange(*start, *duration);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] float rec709_to_linear(const float encoded) {
  const float value = std::clamp(encoded, 0.0F, 1.0F);
  if (value < 0.081F) {
    return value / 4.5F;
  }
  return std::pow((value + 0.099F) / 1.099F, 1.0F / 0.45F);
}

[[nodiscard]] render::RenderResult<std::shared_ptr<const render::CpuFrame>>
convert_frame(DecodeSession& session, const AVFrame& source, const int preferred_width,
              const int preferred_height) {
  const int source_width = source.width > 0 ? source.width : session.decoder->width;
  const int source_height = source.height > 0 ? source.height : session.decoder->height;
  const int output_width =
      preferred_width > 0 && preferred_height > 0 ? preferred_width : source_width;
  const int output_height =
      preferred_width > 0 && preferred_height > 0 ? preferred_height : source_height;
  if (source_width <= 0 || source_height <= 0 || output_width <= 0 || output_height <= 0 ||
      output_width > kMaximumOutputDimension || output_height > kMaximumOutputDimension) {
    return failure<std::shared_ptr<const render::CpuFrame>>(
        render::RenderErrorCode::ProviderFailure, "decoded frame has invalid or unsafe dimensions");
  }

  const auto source_format = static_cast<AVPixelFormat>(source.format);
  if (source_format == AV_PIX_FMT_NONE) {
    return failure<std::shared_ptr<const render::CpuFrame>>(
        render::RenderErrorCode::ProviderFailure, "decoded frame has no pixel format");
  }

  SwsContext* previous = session.scaler.release();
  SwsContext* updated = sws_getCachedContext(
      previous, source_width, source_height, source_format, output_width, output_height,
      AV_PIX_FMT_RGBA64, SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
  session.scaler.reset(updated);
  if (updated == nullptr) {
    return failure<std::shared_ptr<const render::CpuFrame>>(
        render::RenderErrorCode::ProviderFailure,
        "cannot initialize the Rec.709 CPU color converter");
  }

  const AVPixFmtDescriptor* pixel_description = av_pix_fmt_desc_get(source_format);
  if (pixel_description != nullptr && (pixel_description->flags & AV_PIX_FMT_FLAG_RGB) == 0) {
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    const int source_full_range = source.color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    const int color_result = sws_setColorspaceDetails(updated, coefficients, source_full_range,
                                                      coefficients, 1, 0, 1 << 16, 1 << 16);
    if (color_result < 0) {
      return failure<std::shared_ptr<const render::CpuFrame>>(
          render::RenderErrorCode::ProviderFailure,
          ffmpeg_message("cannot configure the Rec.709 CPU color converter", color_result));
    }
  }

  const auto pixel_count =
      static_cast<std::size_t>(output_width) * static_cast<std::size_t>(output_height);
  if (pixel_count > std::numeric_limits<std::size_t>::max() / (4U * sizeof(std::uint16_t))) {
    return failure<std::shared_ptr<const render::CpuFrame>>(
        render::RenderErrorCode::ProviderFailure, "converted frame allocation would overflow");
  }

  try {
    std::vector<std::uint16_t> encoded_rgba(pixel_count * 4U);
    const std::array<const std::uint8_t*, AV_NUM_DATA_POINTERS> source_data{
        source.data[0], source.data[1], source.data[2], source.data[3],
        source.data[4], source.data[5], source.data[6], source.data[7]};
    const std::array<int, AV_NUM_DATA_POINTERS> source_linesize{
        source.linesize[0], source.linesize[1], source.linesize[2], source.linesize[3],
        source.linesize[4], source.linesize[5], source.linesize[6], source.linesize[7]};
    std::array<std::uint8_t*, 4> destination_data{
        reinterpret_cast<std::uint8_t*>(encoded_rgba.data()), nullptr, nullptr, nullptr};
    std::array<int, 4> destination_linesize{
        output_width * static_cast<int>(4U * sizeof(std::uint16_t)), 0, 0, 0};
    const int rows = sws_scale(updated, source_data.data(), source_linesize.data(), 0,
                               source_height, destination_data.data(), destination_linesize.data());
    if (rows != output_height) {
      return failure<std::shared_ptr<const render::CpuFrame>>(
          render::RenderErrorCode::ProviderFailure,
          rows < 0 ? ffmpeg_message("cannot convert the decoded video frame", rows)
                   : "CPU color converter produced an incomplete frame");
    }

    auto output = std::make_shared<render::CpuFrame>(output_width, output_height);
    auto pixels = output->pixels();
    constexpr float kScale = 1.0F / 65'535.0F;
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
      const std::size_t offset = pixel * 4U;
      pixels[offset] = rec709_to_linear(static_cast<float>(encoded_rgba[offset]) * kScale);
      pixels[offset + 1U] =
          rec709_to_linear(static_cast<float>(encoded_rgba[offset + 1U]) * kScale);
      pixels[offset + 2U] =
          rec709_to_linear(static_cast<float>(encoded_rgba[offset + 2U]) * kScale);
      pixels[offset + 3U] =
          std::clamp(static_cast<float>(encoded_rgba[offset + 3U]) * kScale, 0.0F, 1.0F);
    }
    return render::RenderResult<std::shared_ptr<const render::CpuFrame>>::success(
        std::static_pointer_cast<const render::CpuFrame>(output));
  } catch (const std::bad_alloc&) {
    return failure<std::shared_ptr<const render::CpuFrame>>(
        render::RenderErrorCode::ProviderFailure,
        "not enough memory to convert the decoded video frame");
  } catch (const std::exception& exception) {
    return failure<std::shared_ptr<const render::CpuFrame>>(
        render::RenderErrorCode::ProviderFailure, exception.what());
  }
}

[[nodiscard]] render::RenderResult<std::unique_ptr<DecodeSession>>
open_session(const ResolvedAssetStream& source, const std::atomic<std::uint64_t>& current_epoch,
             const std::uint64_t request_epoch) {
  if (!media::media_uri_exists(source.location.path)) {
    return failure<std::unique_ptr<DecodeSession>>(render::RenderErrorCode::AssetUnavailable,
                                                   "playback media file is unavailable: " +
                                                       source.location.path.string());
  }

  auto session = std::make_unique<DecodeSession>();
  session->source = source;
  session->interrupt.current_epoch = &current_epoch;
  session->interrupt.request_epoch.store(request_epoch, std::memory_order_relaxed);
  media::install_quiet_ffmpeg_log_filter();
  session->format.reset(avformat_alloc_context());
  if (!session->format) {
    return failure<std::unique_ptr<DecodeSession>>(render::RenderErrorCode::ProviderFailure,
                                                   "cannot allocate an FFmpeg format context");
  }
  session->format->interrupt_callback = {.callback = interrupt_callback,
                                         .opaque = &session->interrupt};
  media::apply_input_probe_options(*session->format);

  AVFormatContext* raw_format = session->format.release();
  const int open_result = media::open_media_input(&raw_format, source.location.path);
  session->format.reset(raw_format);
  if (open_result < 0) {
    return failure<std::unique_ptr<DecodeSession>>(
        stale(*session) ? render::RenderErrorCode::StaleRequest
                        : render::RenderErrorCode::AssetUnavailable,
        ffmpeg_message("cannot open playback media", open_result));
  }

  const int information_result = media::inspect_input_streams(*session->format);
  if (information_result < 0) {
    return failure<std::unique_ptr<DecodeSession>>(
        stale(*session) ? render::RenderErrorCode::StaleRequest
                        : render::RenderErrorCode::ProviderFailure,
        ffmpeg_message("cannot read playback stream information", information_result));
  }

  int stream_index = source.location.video_stream_index;
  if (stream_index < 0) {
    stream_index =
        av_find_best_stream(session->format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  }
  if (stream_index < 0 || static_cast<unsigned>(stream_index) >= session->format->nb_streams) {
    return failure<std::unique_ptr<DecodeSession>>(render::RenderErrorCode::AssetUnavailable,
                                                   "registered media has no selected video stream");
  }
  session->stream_index = stream_index;
  session->stream = session->format->streams[stream_index];
  if (session->stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
    return failure<std::unique_ptr<DecodeSession>>(render::RenderErrorCode::AssetUnavailable,
                                                   "registered playback stream is not video");
  }

  const AVCodec* codec = avcodec_find_decoder(session->stream->codecpar->codec_id);
  if (codec == nullptr) {
    return failure<std::unique_ptr<DecodeSession>>(
        render::RenderErrorCode::ProviderFailure,
        "no decoder is available for the selected video stream");
  }
  session->decoder.reset(avcodec_alloc_context3(codec));
  if (!session->decoder) {
    return failure<std::unique_ptr<DecodeSession>>(render::RenderErrorCode::ProviderFailure,
                                                   "cannot allocate an FFmpeg video decoder");
  }
  const int parameter_result =
      avcodec_parameters_to_context(session->decoder.get(), session->stream->codecpar);
  if (parameter_result < 0) {
    return failure<std::unique_ptr<DecodeSession>>(
        render::RenderErrorCode::ProviderFailure,
        ffmpeg_message("cannot configure the video decoder", parameter_result));
  }
  session->decoder->pkt_timebase = session->stream->time_base;
  const int decoder_result = avcodec_open2(session->decoder.get(), codec, nullptr);
  if (decoder_result < 0) {
    return failure<std::unique_ptr<DecodeSession>>(
        render::RenderErrorCode::ProviderFailure,
        ffmpeg_message("cannot open the video decoder", decoder_result));
  }

  session->packet.reset(av_packet_alloc());
  session->scratch_frame.reset(av_frame_alloc());
  if (!session->packet || !session->scratch_frame) {
    return failure<std::unique_ptr<DecodeSession>>(render::RenderErrorCode::ProviderFailure,
                                                   "cannot allocate FFmpeg decode buffers");
  }

  if (session->stream->start_time != AV_NOPTS_VALUE) {
    session->origin_pts = session->stream->start_time;
  } else if (session->format->start_time != AV_NOPTS_VALUE) {
    session->origin_pts =
        av_rescale_q(session->format->start_time, AV_TIME_BASE_Q, session->stream->time_base);
  }

  if (source.is_proxy) {
    if (source.pts_map == nullptr) {
      return failure<std::unique_ptr<DecodeSession>>(
          render::RenderErrorCode::AssetUnavailable,
          "proxy playback requires a validated PTS map");
    }
    session->mapped_stream =
        proxy::stream_pts_map(*source.pts_map, source.location.video_stream_index);
    if (session->mapped_stream == nullptr || session->mapped_stream->frames.empty()) {
      return failure<std::unique_ptr<DecodeSession>>(
          render::RenderErrorCode::AssetUnavailable,
          "proxy playback PTS map has no mapping for the selected video stream");
    }
  }

  return render::RenderResult<std::unique_ptr<DecodeSession>>::success(std::move(session));
}

[[nodiscard]] render::RenderResult<DecodedAssetFrame>
decode_at(DecodeSession& session, const render::AssetFrameRequest& request,
          PlaybackStatistics& statistics) {
  session.interrupt.request_epoch.store(request.request_epoch, std::memory_order_relaxed);
  if (stale(session)) {
    return failure<DecodedAssetFrame>(render::RenderErrorCode::StaleRequest,
                                      "playback request belongs to a stale epoch");
  }

  if (session.current_frame.has_value() &&
      session.current_frame->presentation.contains(request.source_time)) {
    ++statistics.cached_frame_requests;
    auto converted = convert_frame(session, *session.current_frame->frame, request.preferred_width,
                                   request.preferred_height);
    if (!converted) {
      return failure<DecodedAssetFrame>(converted.error->code, converted.error->message);
    }
    return render::RenderResult<DecodedAssetFrame>::success(
        {.pixels = *converted.value,
         .presentation = session.current_frame->presentation,
         .used_proxy = session.source.is_proxy,
         .video_stream_index = session.stream_index});
  }

  bool decode_sequentially = false;
  if (session.current_frame.has_value() &&
      request.source_time >= session.current_frame->presentation.end()) {
    const edit::Time gap = request.source_time - session.current_frame->presentation.end();
    decode_sequentially = gap <= edit::Time(kMaximumSequentialGapSeconds, 1);
  }

  if (decode_sequentially) {
    ++statistics.sequential_requests;
  } else if (session.mapped_stream != nullptr) {
    const AVRational source_time_base = pts_time_base(session.mapped_stream->source_time_base);
    const auto relative_target = stream_ticks_for_time(request.source_time, source_time_base);
    if (!relative_target.has_value() ||
        add_would_overflow(session.mapped_stream->source_origin_pts, *relative_target)) {
      return failure<DecodedAssetFrame>(
          render::RenderErrorCode::InvalidTime,
          "requested source time cannot be represented in the proxy PTS map");
    }
    const std::int64_t absolute_source_pts =
        session.mapped_stream->source_origin_pts + *relative_target;
    const auto mapping =
        proxy::lookup_frame_by_source_pts(*session.mapped_stream, absolute_source_pts);
    if (!mapping.has_value()) {
      return failure<DecodedAssetFrame>(render::RenderErrorCode::AssetUnavailable,
                                        "no proxy PTS map frame covers the requested source time");
    }
    const std::int64_t seek_target = mapping->proxy_pts;
    const int seek_result = av_seek_frame(session.format.get(), session.stream_index, seek_target,
                                          AVSEEK_FLAG_BACKWARD);
    if (seek_result < 0) {
      return failure<DecodedAssetFrame>(
          stale(session) ? render::RenderErrorCode::StaleRequest
                         : render::RenderErrorCode::ProviderFailure,
          ffmpeg_message("cannot seek to the mapped proxy keyframe", seek_result));
    }
    ++statistics.seeks;
    avformat_flush(session.format.get());
    avcodec_flush_buffers(session.decoder.get());
    session.draining = false;
    session.queued_frame.reset();
    session.current_frame.reset();
  } else {
    const auto relative_target =
        stream_ticks_for_time(request.source_time, session.stream->time_base);
    if (!relative_target.has_value() || add_would_overflow(session.origin_pts, *relative_target)) {
      return failure<DecodedAssetFrame>(
          render::RenderErrorCode::InvalidTime,
          "requested source time cannot be represented in the stream time base");
    }
    const std::int64_t seek_target = session.origin_pts + *relative_target;
    const int seek_result = av_seek_frame(session.format.get(), session.stream_index, seek_target,
                                          AVSEEK_FLAG_BACKWARD);
    if (seek_result < 0) {
      return failure<DecodedAssetFrame>(
          stale(session) ? render::RenderErrorCode::StaleRequest
                         : render::RenderErrorCode::ProviderFailure,
          ffmpeg_message("cannot seek to the preceding video keyframe", seek_result));
    }
    ++statistics.seeks;
    avformat_flush(session.format.get());
    avcodec_flush_buffers(session.decoder.get());
    session.draining = false;
    session.queued_frame.reset();
    session.current_frame.reset();
  }

  for (;;) {
    FrameReadResult candidate = take_next_frame(session, statistics);
    if (candidate.status == ReadStatus::Stale) {
      return failure<DecodedAssetFrame>(render::RenderErrorCode::StaleRequest, candidate.message);
    }
    if (candidate.status == ReadStatus::Failure) {
      return failure<DecodedAssetFrame>(render::RenderErrorCode::ProviderFailure,
                                        ffmpeg_message(candidate.message, candidate.native_code));
    }
    if (candidate.status == ReadStatus::EndOfStream) {
      if (session.current_frame.has_value()) {
        StoredFrame& held = *session.current_frame;
        if (request.source_time >= held.presentation.start) {
          const edit::Time needed =
              request.source_time - held.presentation.start +
              edit::Time(1, std::max<std::uint32_t>(1U, request.source_time.timescale()));
          if (needed > held.presentation.duration) {
            held.presentation.duration = needed;
          }
          auto converted =
              convert_frame(session, *held.frame, request.preferred_width, request.preferred_height);
          if (!converted) {
            return failure<DecodedAssetFrame>(converted.error->code, converted.error->message);
          }
          return render::RenderResult<DecodedAssetFrame>::success(
              {.pixels = *converted.value,
               .presentation = held.presentation,
               .used_proxy = session.source.is_proxy,
               .video_stream_index = session.stream_index});
        }
      }
      return failure<DecodedAssetFrame>(render::RenderErrorCode::AssetUnavailable,
                                        "no decoded video frame covers the requested source time");
    }

    if (candidate.frame.duration_ticks <= 0) {
      if (session.mapped_stream != nullptr) {
        if (const auto mapping = proxy::lookup_frame_by_proxy_pts(*session.mapped_stream,
                                                                  candidate.frame.timestamp)) {
          candidate.frame.duration_ticks = mapping->proxy_duration;
        }
      }
      if (candidate.frame.duration_ticks <= 0) {
      FrameReadResult following = take_next_frame(session, statistics);
      if (following.status == ReadStatus::Stale) {
        return failure<DecodedAssetFrame>(render::RenderErrorCode::StaleRequest, following.message);
      }
      if (following.status == ReadStatus::Failure) {
        return failure<DecodedAssetFrame>(render::RenderErrorCode::ProviderFailure,
                                          ffmpeg_message(following.message, following.native_code));
      }
      if (following.status == ReadStatus::Frame) {
        if (following.frame.timestamp <= candidate.frame.timestamp) {
          return failure<DecodedAssetFrame>(
              render::RenderErrorCode::ProviderFailure,
              "decoded frames without durations are not in increasing presentation order");
        }
        candidate.frame.duration_ticks = following.frame.timestamp - candidate.frame.timestamp;
        session.queued_frame = std::move(following.frame);
      } else {
        if (session.stream->duration == AV_NOPTS_VALUE ||
            add_would_overflow(session.origin_pts, session.stream->duration)) {
          return failure<DecodedAssetFrame>(
              render::RenderErrorCode::ProviderFailure,
              "the final decoded frame has no exact presentation duration");
        }
        const std::int64_t stream_end = session.origin_pts + session.stream->duration;
        if (stream_end <= candidate.frame.timestamp) {
          return failure<DecodedAssetFrame>(
              render::RenderErrorCode::ProviderFailure,
              "the final decoded frame is outside the stream duration");
        }
        candidate.frame.duration_ticks = stream_end - candidate.frame.timestamp;
      }
      }
    }

    std::optional<edit::TimeRange> presentation;
    if (session.mapped_stream != nullptr) {
      const auto mapping =
          proxy::lookup_frame_by_proxy_pts(*session.mapped_stream, candidate.frame.timestamp);
      if (!mapping.has_value()) {
        continue;
      }
      presentation = presentation_for_mapping(*session.mapped_stream, *mapping);
    } else {
      presentation =
          presentation_for_frame(session, candidate.frame.timestamp, candidate.frame.duration_ticks);
    }
    if (!presentation.has_value()) {
      return failure<DecodedAssetFrame>(
          render::RenderErrorCode::ProviderFailure,
          "decoded video frame has no usable presentation timestamp or duration");
    }
    if (request.source_time < presentation->start) {
      return failure<DecodedAssetFrame>(
          render::RenderErrorCode::AssetUnavailable,
          "requested source time precedes the first available video frame");
    }
    if (!presentation->contains(request.source_time)) {
      session.current_frame =
          StoredFrame{.frame = std::move(candidate.frame.frame), .presentation = *presentation};
      continue;
    }

    session.current_frame =
        StoredFrame{.frame = std::move(candidate.frame.frame), .presentation = *presentation};
    auto converted = convert_frame(session, *session.current_frame->frame, request.preferred_width,
                                   request.preferred_height);
    if (!converted) {
      return failure<DecodedAssetFrame>(converted.error->code, converted.error->message);
    }
    return render::RenderResult<DecodedAssetFrame>::success(
        {.pixels = *converted.value,
         .presentation = session.current_frame->presentation,
         .used_proxy = session.source.is_proxy,
         .video_stream_index = session.stream_index});
  }
}

} // namespace

class FfmpegFrameProvider::Impl final {
public:
  explicit Impl(std::shared_ptr<AssetRegistry> source_registry)
      : registry(std::move(source_registry)) {}

  std::shared_ptr<AssetRegistry> registry;
  // Declared before sessions so callback targets outlive all format contexts.
  std::atomic<std::uint64_t> epoch{0};
  mutable std::mutex mutex;
  std::unordered_map<edit::EntityId, std::unique_ptr<DecodeSession>> sessions;
  PlaybackStatistics statistics;
};

FfmpegFrameProvider::FfmpegFrameProvider(std::shared_ptr<AssetRegistry> registry)
    : impl_(std::make_unique<Impl>(std::move(registry))) {
  if (!impl_->registry) {
    throw std::invalid_argument("FFmpeg frame provider requires an asset registry");
  }
}

FfmpegFrameProvider::~FfmpegFrameProvider() = default;

void FfmpegFrameProvider::begin_epoch(const std::uint64_t request_epoch) noexcept {
  impl_->epoch.store(request_epoch, std::memory_order_release);
}

std::uint64_t FfmpegFrameProvider::current_epoch() const noexcept {
  return impl_->epoch.load(std::memory_order_acquire);
}

render::RenderResult<std::shared_ptr<const render::CpuFrame>>
FfmpegFrameProvider::request(const render::AssetFrameRequest& request_value) {
  auto detailed = request_with_timing(request_value);
  if (!detailed) {
    return failure<std::shared_ptr<const render::CpuFrame>>(detailed.error->code,
                                                            detailed.error->message);
  }
  return render::RenderResult<std::shared_ptr<const render::CpuFrame>>::success(
      detailed.value->pixels);
}

render::RenderResult<DecodedAssetFrame>
FfmpegFrameProvider::request_with_timing(const render::AssetFrameRequest& request) {
  if (request.request_epoch != current_epoch()) {
    return failure<DecodedAssetFrame>(render::RenderErrorCode::StaleRequest,
                                      "playback request belongs to a stale epoch");
  }
  if (request.source_time.isNegative()) {
    return failure<DecodedAssetFrame>(render::RenderErrorCode::InvalidTime,
                                      "cannot decode negative source time");
  }

  const auto source = impl_->registry->resolve(request.asset_id, request.permit_proxy);
  if (!source.has_value()) {
    return failure<DecodedAssetFrame>(render::RenderErrorCode::AssetUnavailable,
                                      "asset is not registered for playback");
  }

  std::unique_lock lock(impl_->mutex);
  if (request.request_epoch != current_epoch()) {
    return failure<DecodedAssetFrame>(render::RenderErrorCode::StaleRequest,
                                      "playback request was superseded before decoding began");
  }

  auto iterator = impl_->sessions.find(request.asset_id);
  if (iterator != impl_->sessions.end() && !same_source(iterator->second->source, *source)) {
    impl_->sessions.erase(iterator);
    iterator = impl_->sessions.end();
  }

  if (iterator == impl_->sessions.end()) {
    auto opened = open_session(*source, impl_->epoch, request.request_epoch);
    if (!opened) {
      return failure<DecodedAssetFrame>(opened.error->code, opened.error->message);
    }
    ++impl_->statistics.sessions_opened;
    iterator = impl_->sessions.emplace(request.asset_id, std::move(*opened.value)).first;
  }

  auto decoded = decode_at(*iterator->second, request, impl_->statistics);
  if (decoded) {
    return decoded;
  }
  if (decoded.error->code == render::RenderErrorCode::StaleRequest) {
    impl_->sessions.erase(iterator);
    return decoded;
  }
  if (decoded.error->code != render::RenderErrorCode::ProviderFailure) {
    return decoded;
  }

  // Demuxers and decoders are stateful. Never continue from a context after an
  // I/O, seek, decode, timestamp, or conversion failure: reopen and retry once.
  impl_->sessions.erase(iterator);
  if (request.request_epoch != current_epoch()) {
    return failure<DecodedAssetFrame>(
        render::RenderErrorCode::StaleRequest,
        "playback request was superseded while recovering the decoder");
  }
  ++impl_->statistics.sessions_reopened;
  auto reopened = open_session(*source, impl_->epoch, request.request_epoch);
  if (!reopened) {
    return failure<DecodedAssetFrame>(reopened.error->code, reopened.error->message);
  }
  ++impl_->statistics.sessions_opened;
  auto [reopened_iterator, inserted] =
      impl_->sessions.emplace(request.asset_id, std::move(*reopened.value));
  static_cast<void>(inserted);
  auto retried = decode_at(*reopened_iterator->second, request, impl_->statistics);
  if (!retried && (retried.error->code == render::RenderErrorCode::StaleRequest ||
                   retried.error->code == render::RenderErrorCode::ProviderFailure)) {
    impl_->sessions.erase(reopened_iterator);
  }
  return retried;
}

void FfmpegFrameProvider::invalidate(const edit::EntityId& asset_id) {
  std::scoped_lock lock(impl_->mutex);
  impl_->sessions.erase(asset_id);
}

void FfmpegFrameProvider::clear_sessions() {
  std::scoped_lock lock(impl_->mutex);
  impl_->sessions.clear();
}

PlaybackStatistics FfmpegFrameProvider::statistics() const noexcept {
  std::scoped_lock lock(impl_->mutex);
  return impl_->statistics;
}

} // namespace video_editor::playback
