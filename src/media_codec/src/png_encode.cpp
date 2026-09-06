// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/png_encode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace video_editor::media {
namespace {

struct FrameDeleter {
  void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};

struct PacketDeleter {
  void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

[[nodiscard]] MediaError make_error(const MediaErrorCode code, const std::string& message,
                                    const int native_code = 0) {
  return {.code = code, .native_code = native_code, .message = message};
}

[[nodiscard]] MediaError ffmpeg_error(const MediaErrorCode code, const std::string& context,
                                      const int status) {
  return make_error(code, context + " failed", status);
}

} // namespace

Result<std::vector<std::uint8_t>> encode_png_rgba8(const std::span<const std::uint8_t> rgba,
                                                    const int width, const int height) {
  if (width <= 0 || height <= 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(MediaErrorCode::InvalidArgument, "PNG dimensions must be positive"));
  }
  const auto expected_size =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  if (rgba.size() != expected_size) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(MediaErrorCode::InvalidArgument, "RGBA buffer size does not match dimensions"));
  }

  const AVCodec* png_encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
  if (png_encoder == nullptr) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(MediaErrorCode::Unsupported, "no PNG encoder available"));
  }

  std::unique_ptr<AVCodecContext, CodecContextDeleter> encoder_context(
      avcodec_alloc_context3(png_encoder));
  if (!encoder_context) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(MediaErrorCode::Internal, "cannot allocate PNG encoder context", AVERROR(ENOMEM)));
  }
  encoder_context->pix_fmt = AV_PIX_FMT_RGBA;
  encoder_context->width = width;
  encoder_context->height = height;
  encoder_context->time_base = AVRational{1, 1};
  encoder_context->thread_count = 1;

  const int open_result = avcodec_open2(encoder_context.get(), png_encoder, nullptr);
  if (open_result < 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        ffmpeg_error(MediaErrorCode::Internal, "open PNG encoder", open_result));
  }

  std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
  if (!frame) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(MediaErrorCode::Internal, "cannot allocate PNG frame", AVERROR(ENOMEM)));
  }
  frame->format = AV_PIX_FMT_RGBA;
  frame->width = width;
  frame->height = height;
  const int buffer_result = av_frame_get_buffer(frame.get(), 0);
  if (buffer_result < 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        ffmpeg_error(MediaErrorCode::Internal, "allocate PNG frame buffer", buffer_result));
  }

  for (int y = 0; y < height; ++y) {
    const auto* row = rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U);
    std::copy_n(row, static_cast<std::size_t>(width) * 4U, frame->data[0] + (y * frame->linesize[0]));
  }
  frame->pts = 0;

  const int send_result = avcodec_send_frame(encoder_context.get(), frame.get());
  if (send_result < 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        ffmpeg_error(MediaErrorCode::Internal, "send frame to PNG encoder", send_result));
  }

  std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
  if (!packet) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(MediaErrorCode::Internal, "cannot allocate PNG packet", AVERROR(ENOMEM)));
  }

  std::vector<std::uint8_t> png_bytes;
  for (;;) {
    const int receive_result = avcodec_receive_packet(encoder_context.get(), packet.get());
    if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
      break;
    }
    if (receive_result < 0) {
      return Result<std::vector<std::uint8_t>>::failure(
          ffmpeg_error(MediaErrorCode::Internal, "encode PNG packet", receive_result));
    }
    png_bytes.insert(png_bytes.end(), packet->data, packet->data + packet->size);
    av_packet_unref(packet.get());
  }

  if (png_bytes.empty()) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(MediaErrorCode::Internal, "PNG encoder produced no output"));
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(png_bytes));
}

} // namespace video_editor::media
