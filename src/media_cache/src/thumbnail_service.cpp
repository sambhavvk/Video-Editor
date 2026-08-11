// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_cache/thumbnail_service.h"

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
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace video_editor::media_cache {
namespace {

// Microseconds time base, matching the convention used elsewhere in the
// project (AV_TIME_BASE is microseconds in FFmpeg).
constexpr AVRational kMicrosecondsTimeBase{1, AV_TIME_BASE};

// ---------------------------------------------------------------------------
// RAII deleters for FFmpeg objects. Mirrors the patterns in
// src/proxy_service/src/proxy_service.cpp.
// ---------------------------------------------------------------------------
struct InputFormatDeleter {
  void operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
      AVFormatContext* local = context;
      avformat_close_input(&local);
    }
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

using InputFormat = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using CodecContext = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using ScaleContext = std::unique_ptr<SwsContext, SwsDeleter>;

// ---------------------------------------------------------------------------
// Error helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string ffmpeg_error_string(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error";
  }
  return std::string(buffer.data());
}

[[nodiscard]] ThumbnailError make_error(ThumbnailErrorCode code, std::string message,
                                        int native_code = 0) {
  return ThumbnailError{.code = code, .native_code = native_code, .message = std::move(message)};
}

[[nodiscard]] ThumbnailError ffmpeg_error(ThumbnailErrorCode code, std::string_view action,
                                          int native_code) {
  return make_error(code, std::string(action) + ": " + ffmpeg_error_string(native_code),
                    native_code);
}

[[nodiscard]] std::string native_path(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
#else
  return path.string();
#endif
}

// ---------------------------------------------------------------------------
// Cancellation plumbing, mirroring proxy_service's InterruptState.
// ---------------------------------------------------------------------------

struct InterruptState {
  std::stop_token cancellation;
};

int interrupt_callback(void* opaque) noexcept {
  const auto* state = static_cast<const InterruptState*>(opaque);
  return state != nullptr && state->cancellation.stop_requested() ? 1 : 0;
}

[[nodiscard]] bool cancelled(const std::stop_token& token) noexcept {
  return token.stop_requested();
}

// ---------------------------------------------------------------------------
// Pure resolvers.
// ---------------------------------------------------------------------------

[[nodiscard]] int even_align(int value) noexcept {
  // Round down to the nearest even, never negative.
  if (value < 0) {
    return 0;
  }
  return value - (value % 2);
}

} // namespace

std::pair<int, int> thumbnail_target_dimensions(const media::VideoDescription& video,
                                                 int maximum_width) noexcept {
  const int source_w = video.width > 0 ? video.width : 0;
  const int source_h = video.height > 0 ? video.height : 0;
  if (source_w <= 0 || source_h <= 0) {
    return {0, 0};
  }
  if (maximum_width <= 0) {
    return {even_align(source_w), even_align(source_h)};
  }

  const int long_edge = std::max(source_w, source_h);
  if (long_edge <= maximum_width) {
    // Do not upscale; keep original dimensions, even-aligned.
    return {even_align(source_w), even_align(source_h)};
  }

  const double scale = static_cast<double>(maximum_width) / static_cast<double>(long_edge);
  const int target_w = even_align(static_cast<int>(std::floor(source_w * scale)));
  const int target_h = even_align(static_cast<int>(std::floor(source_h * scale)));
  return {target_w, target_h};
}

std::int64_t thumbnail_source_pts(std::optional<std::int64_t> duration_microseconds,
                                   ThumbnailOptions::Strategy strategy) noexcept {
  if (!duration_microseconds.has_value()) {
    return 0;
  }
  const std::int64_t duration = *duration_microseconds;
  if (duration <= 0) {
    return 0;
  }
  switch (strategy) {
    case ThumbnailOptions::Strategy::First:
      return 0;
    case ThumbnailOptions::Strategy::Middle:
      return duration / 2;
    case ThumbnailOptions::Strategy::Last:
      return duration - 1;
  }
  return 0;
}

std::string thumbnail_parameter_hash(const ThumbnailOptions& options) {
  // Pack maximum_width, strategy (as int), quality into a short deterministic
  // string. No crypto: this is a cache key, not a security boundary.
  char buffer[64];
  const int strategy_int = static_cast<int>(options.strategy);
  const int written = std::snprintf(buffer, sizeof(buffer), "w%dm%dq%d",
                                     options.maximum_width, strategy_int, options.quality);
  if (written < 0) {
    return {};
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

namespace {

// ---------------------------------------------------------------------------
// FFmpeg object factories.
// ---------------------------------------------------------------------------

[[nodiscard]] Frame make_frame() {
  Frame frame(av_frame_alloc());
  if (!frame) {
    return nullptr;
  }
  return frame;
}

[[nodiscard]] Packet make_packet() {
  Packet packet(av_packet_alloc());
  if (!packet) {
    return nullptr;
  }
  return packet;
}

// ---------------------------------------------------------------------------
// FFmpeg-dependent generation. Returns a ThumbnailResult; never throws.
// ---------------------------------------------------------------------------

[[nodiscard]] ThumbnailResult<Thumbnail>
generate_from_source(const std::filesystem::path& asset_uri, int stream_index,
                     const ThumbnailOptions& options, const std::string& asset_id,
                     CacheStore& cache, std::stop_token cancellation) {
  if (options.maximum_width <= 0) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::InvalidArgument, "maximum_width must be positive"));
  }
  if (options.quality < 1 || options.quality > 100) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::InvalidArgument, "quality must be in [1, 100]"));
  }
  if (asset_id.empty()) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::InvalidArgument, "asset_id must not be empty"));
  }
  if (asset_uri.empty()) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::InvalidArgument, "asset_uri must not be empty"));
  }

  if (cancelled(cancellation)) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                   AVERROR_EXIT));
  }

  if (!std::filesystem::exists(asset_uri)) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::SourceNotFound, "asset does not exist"));
  }

  // --- Open input -----------------------------------------------------------
  InterruptState interrupt{cancellation};
  AVFormatContext* raw_input = avformat_alloc_context();
  if (raw_input == nullptr) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Internal, "cannot allocate input format context",
                   AVERROR(ENOMEM)));
  }
  raw_input->interrupt_callback = {.callback = interrupt_callback, .opaque = &interrupt};
  const std::string path = native_path(asset_uri);
  const int open_result = avformat_open_input(&raw_input, path.c_str(), nullptr, nullptr);
  InputFormat input(raw_input);
  if (open_result < 0) {
    if (cancelled(cancellation)) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                     AVERROR_EXIT));
    }
    return ThumbnailResult<Thumbnail>::failure(
        ffmpeg_error(ThumbnailErrorCode::OpenFailed, "open asset", open_result));
  }

  const int stream_info_result = avformat_find_stream_info(input.get(), nullptr);
  if (stream_info_result < 0) {
    if (cancelled(cancellation)) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                     AVERROR_EXIT));
    }
    return ThumbnailResult<Thumbnail>::failure(
        ffmpeg_error(ThumbnailErrorCode::OpenFailed, "inspect asset streams", stream_info_result));
  }

  // --- Select video stream -------------------------------------------------
  if (cancelled(cancellation)) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                   AVERROR_EXIT));
  }

  int selected_index = stream_index;
  if (selected_index < 0) {
    selected_index =
        av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (selected_index < 0) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::NoVideoStream, "asset has no decodable video stream",
                     selected_index));
    }
  } else {
    if (selected_index >= static_cast<int>(input->nb_streams)) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::NoVideoStream, "requested stream index is out of range"));
    }
    if (input->streams[static_cast<unsigned>(selected_index)]->codecpar->codec_type !=
        AVMEDIA_TYPE_VIDEO) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::NoVideoStream,
                     "requested stream index is not a video stream"));
    }
  }

  AVStream* stream = input->streams[static_cast<unsigned>(selected_index)];

  // --- Open decoder ---------------------------------------------------------
  const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (decoder == nullptr) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::DecodeFailed, "no decoder available for the video stream"));
  }
  CodecContext decoder_context(avcodec_alloc_context3(decoder));
  if (!decoder_context) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Internal, "cannot allocate decoder context",
                   AVERROR(ENOMEM)));
  }
  {
    const int params_result = avcodec_parameters_to_context(decoder_context.get(), stream->codecpar);
    if (params_result < 0) {
      return ThumbnailResult<Thumbnail>::failure(
          ffmpeg_error(ThumbnailErrorCode::DecodeFailed, "copy decoder parameters", params_result));
    }
  }
  decoder_context->pkt_timebase = stream->time_base;
  {
    const int open_dec_result = avcodec_open2(decoder_context.get(), decoder, nullptr);
    if (open_dec_result < 0) {
      return ThumbnailResult<Thumbnail>::failure(
          ffmpeg_error(ThumbnailErrorCode::DecodeFailed, "open decoder", open_dec_result));
    }
  }

  // --- Compute target dimensions & seek target ------------------------------
  media::VideoDescription video_desc;
  video_desc.width = decoder_context->width;
  video_desc.height = decoder_context->height;
  const auto [target_w, target_h] = thumbnail_target_dimensions(video_desc, options.maximum_width);
  if (target_w <= 0 || target_h <= 0) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::InvalidArgument,
                   "computed target dimensions are non-positive"));
  }

  // Source duration in microseconds.
  std::optional<std::int64_t> duration_us;
  if (input->duration > 0) {
    duration_us = input->duration;
  } else if (stream->duration > 0 && stream->time_base.den > 0) {
    duration_us = av_rescale_q(stream->duration, stream->time_base, kMicrosecondsTimeBase);
  }
  const std::int64_t target_pts_us = thumbnail_source_pts(duration_us, options.strategy);

  // Convert microseconds → stream time base for seeking.
  const std::int64_t target_pts_stream =
      stream->time_base.den > 0
          ? av_rescale_q(target_pts_us, kMicrosecondsTimeBase, stream->time_base)
          : 0;

  // --- Seek -----------------------------------------------------------------
  if (target_pts_stream > 0) {
    const int seek_result =
        av_seek_frame(input.get(), selected_index, target_pts_stream, AVSEEK_FLAG_BACKWARD);
    if (seek_result < 0) {
      // Seek is best-effort; fall through and decode from the start if it fails.
      // (Many containers cannot seek to a precise PTS; we still try to land
      // near the target by reading forward.)
    }
  }

  // --- Decode until first usable frame --------------------------------------
  if (cancelled(cancellation)) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                   AVERROR_EXIT));
  }

  Frame decoded = make_frame();
  if (!decoded) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Internal, "cannot allocate decoded frame",
                   AVERROR(ENOMEM)));
  }
  Packet packet = make_packet();
  if (!packet) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Internal, "cannot allocate packet",
                   AVERROR(ENOMEM)));
  }

  // After seeking (or from the start if seeking was skipped), decode forward
  // and accept the first decoded frame. Seeking with AVSEEK_FLAG_BACKWARD lands
  // at or before the target, so the first frame is the closest available
  // representative frame for the chosen strategy.
  bool have_frame = false;
  std::int64_t chosen_pts_us = 0;

  for (;;) {
    if (cancelled(cancellation)) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                     AVERROR_EXIT));
    }
    const int read_result = av_read_frame(input.get(), packet.get());
    if (read_result == AVERROR_EOF) {
      break;
    }
    if (read_result < 0) {
      return ThumbnailResult<Thumbnail>::failure(
          ffmpeg_error(ThumbnailErrorCode::DecodeFailed, "read packet", read_result));
    }
    if (packet->stream_index != selected_index) {
      av_packet_unref(packet.get());
      continue;
    }

    const int send_result = avcodec_send_packet(decoder_context.get(), packet.get());
    av_packet_unref(packet.get());
    if (send_result < 0 && send_result != AVERROR(EAGAIN) && send_result != AVERROR_EOF) {
      return ThumbnailResult<Thumbnail>::failure(
          ffmpeg_error(ThumbnailErrorCode::DecodeFailed, "send packet to decoder", send_result));
    }

    for (;;) {
      if (cancelled(cancellation)) {
        return ThumbnailResult<Thumbnail>::failure(
            make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                     AVERROR_EXIT));
      }
      const int recv_result = avcodec_receive_frame(decoder_context.get(), decoded.get());
      if (recv_result == AVERROR(EAGAIN) || recv_result == AVERROR_EOF) {
        break;
      }
      if (recv_result < 0) {
        return ThumbnailResult<Thumbnail>::failure(
            ffmpeg_error(ThumbnailErrorCode::DecodeFailed, "decode frame", recv_result));
      }

      std::int64_t pts = decoded->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE) {
        pts = decoded->pts;
      }
      if (pts != AV_NOPTS_VALUE && stream->time_base.den > 0) {
        chosen_pts_us = av_rescale_q(pts, stream->time_base, kMicrosecondsTimeBase);
      }
      have_frame = true;
      break;
    }
    if (have_frame) {
      break;
    }
  }

  if (!have_frame) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::DecodeFailed, "no decodable video frame found"));
  }
  // --- Scale to YUV420P at target dimensions --------------------------------
  if (cancelled(cancellation)) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                   AVERROR_EXIT));
  }

  ScaleContext scaler(sws_getCachedContext(
      nullptr, decoder_context->width, decoder_context->height, decoder_context->pix_fmt,
      target_w, target_h, AV_PIX_FMT_YUV420P, SWS_BICUBIC | SWS_FULL_CHR_H_INT, nullptr, nullptr,
      nullptr));
  if (!scaler) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::ScaleFailed, "cannot configure pixel scaler"));
  }

  Frame scaled = make_frame();
  if (!scaled) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Internal, "cannot allocate scaled frame",
                   AVERROR(ENOMEM)));
  }
  scaled->format = AV_PIX_FMT_YUV420P;
  scaled->width = target_w;
  scaled->height = target_h;
  {
    const int alloc_result = av_frame_get_buffer(scaled.get(), 32);
    if (alloc_result < 0) {
      return ThumbnailResult<Thumbnail>::failure(
          ffmpeg_error(ThumbnailErrorCode::ScaleFailed, "allocate scaled frame buffer",
                       alloc_result));
    }
  }

  {
    const int scaled_rows = sws_scale(scaler.get(), decoded->data, decoded->linesize, 0,
                                      decoded->height, scaled->data, scaled->linesize);
    if (scaled_rows != target_h) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::ScaleFailed, "pixel scaler produced an incomplete frame"));
    }
  }

  // --- Encode JPEG ----------------------------------------------------------
  if (cancelled(cancellation)) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                   AVERROR_EXIT));
  }

  const AVCodec* jpeg_encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (jpeg_encoder == nullptr) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::EncodeFailed, "no MJPEG encoder available"));
  }
  CodecContext encoder_context(avcodec_alloc_context3(jpeg_encoder));
  if (!encoder_context) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Internal, "cannot allocate JPEG encoder context",
                   AVERROR(ENOMEM)));
  }
  encoder_context->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder_context->width = target_w;
  encoder_context->height = target_h;
  encoder_context->time_base = AVRational{1, 1};
  encoder_context->thread_count = 1;
  // JPEG quality via the global quality field (qscale = quality / FF_QP2LAMBDA).
  encoder_context->flags |= AV_CODEC_FLAG_QSCALE;
  encoder_context->global_quality = std::max(1, std::min(100, options.quality)) * FF_QP2LAMBDA;
  {
    const int enc_open_result = avcodec_open2(encoder_context.get(), jpeg_encoder, nullptr);
    if (enc_open_result < 0) {
      return ThumbnailResult<Thumbnail>::failure(
          ffmpeg_error(ThumbnailErrorCode::EncodeFailed, "open JPEG encoder", enc_open_result));
    }
  }

  scaled->pts = 0;
  scaled->quality = encoder_context->global_quality;
  {
    const int send_result = avcodec_send_frame(encoder_context.get(), scaled.get());
    if (send_result < 0) {
      return ThumbnailResult<Thumbnail>::failure(
          ffmpeg_error(ThumbnailErrorCode::EncodeFailed, "send frame to JPEG encoder",
                       send_result));
    }
  }

  Packet jpeg_packet = make_packet();
  if (!jpeg_packet) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::Internal, "cannot allocate JPEG packet",
                   AVERROR(ENOMEM)));
  }
  std::vector<std::byte> jpeg_bytes;
  for (;;) {
    if (cancelled(cancellation)) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::Cancelled, "thumbnail generation was cancelled",
                     AVERROR_EXIT));
    }
    const int recv_result = avcodec_receive_packet(encoder_context.get(), jpeg_packet.get());
    if (recv_result == AVERROR(EAGAIN) || recv_result == AVERROR_EOF) {
      break;
    }
    if (recv_result < 0) {
      return ThumbnailResult<Thumbnail>::failure(
          ffmpeg_error(ThumbnailErrorCode::EncodeFailed, "encode JPEG packet", recv_result));
    }
    const auto* data = reinterpret_cast<const std::byte*>(jpeg_packet->data);
    jpeg_bytes.insert(jpeg_bytes.end(), data, data + jpeg_packet->size);
    av_packet_unref(jpeg_packet.get());
  }

  if (jpeg_bytes.empty()) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::EncodeFailed, "JPEG encoder produced no output"));
  }

  // --- Store & return -------------------------------------------------------
  Thumbnail thumbnail;
  thumbnail.jpeg_bytes = std::move(jpeg_bytes);
  thumbnail.width = target_w;
  thumbnail.height = target_h;
  thumbnail.source_pts_microseconds = chosen_pts_us;
  thumbnail.source_stream_index = static_cast<std::int64_t>(selected_index);

  const std::string parameter_hash = thumbnail_parameter_hash(options);
  const CacheKey key{.asset_id = asset_id,
                     .kind = CacheKind::Thumbnail,
                     .parameter_hash = parameter_hash};
  const auto put_result =
      cache.put(key, std::span<const std::byte>(thumbnail.jpeg_bytes.data(),
                                                 thumbnail.jpeg_bytes.size()));
  if (!put_result) {
    const CacheError& err = put_result.error();
    return ThumbnailResult<Thumbnail>::failure(make_error(
        ThumbnailErrorCode::StoreFailed,
        "cannot store thumbnail: " + err.message, err.native_code));
  }

  return ThumbnailResult<Thumbnail>::success(std::move(thumbnail));
}

} // namespace

ThumbnailResult<Thumbnail> generate_thumbnail(const std::filesystem::path& asset_uri,
                                                int stream_index,
                                                const ThumbnailOptions& options,
                                                const std::string& asset_id,
                                                CacheStore& cache,
                                                std::stop_token cancellation) {
  // Fast path: if a valid cached entry already exists, return it without
  // re-decoding. The store's `contains` check is authoritative.
  if (!asset_id.empty()) {
    const std::string parameter_hash = thumbnail_parameter_hash(options);
    const CacheKey key{.asset_id = asset_id,
                       .kind = CacheKind::Thumbnail,
                       .parameter_hash = parameter_hash};
    auto contains_result = cache.contains(key);
    if (contains_result && contains_result.value()) {
      return load_thumbnail(asset_id, options, cache);
    }
    // If contains() failed (e.g. internal store error), fall through to
    // regeneration; the put at the end will surface a StoreFailed error if
    // the store is genuinely broken.
  }

  return generate_from_source(asset_uri, stream_index, options, asset_id, cache,
                              std::move(cancellation));
}

ThumbnailResult<Thumbnail> load_thumbnail(const std::string& asset_id,
                                           const ThumbnailOptions& options,
                                           CacheStore& cache) {
  if (asset_id.empty()) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::InvalidArgument, "asset_id must not be empty"));
  }

  const std::string parameter_hash = thumbnail_parameter_hash(options);
  const CacheKey key{.asset_id = asset_id,
                     .kind = CacheKind::Thumbnail,
                     .parameter_hash = parameter_hash};
  if (!key.valid()) {
    return ThumbnailResult<Thumbnail>::failure(
        make_error(ThumbnailErrorCode::InvalidArgument, "thumbnail cache key is invalid"));
  }

  auto get_result = cache.get(key);
  if (!get_result) {
    const CacheError& err = get_result.error();
    if (err.code == CacheErrorCode::NotFound) {
      return ThumbnailResult<Thumbnail>::failure(
          make_error(ThumbnailErrorCode::NotFound, "thumbnail not found in cache"));
    }
    return ThumbnailResult<Thumbnail>::failure(make_error(
        ThumbnailErrorCode::Internal, "cache read failed: " + err.message,
        err.native_code));
  }

  std::vector<std::byte> bytes = std::move(get_result).value();
  // We do not re-parse the JPEG to recover width/height/pts here; those
  // metadata are not stored alongside the blob in the current cache contract.
  // Callers that need dimensions should regenerate or store a sidecar. We
  // return the bytes with zeroed metadata; the JPEG itself is self-describing
  // for display purposes.
  Thumbnail thumbnail;
  thumbnail.jpeg_bytes = std::move(bytes);
  thumbnail.source_stream_index = -1;
  return ThumbnailResult<Thumbnail>::success(std::move(thumbnail));
}

} // namespace video_editor::media_cache
