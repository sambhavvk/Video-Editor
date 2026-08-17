// SPDX-License-Identifier: MPL-2.0
#include "video_editor/transcription_service/transcription_service.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace video_editor::transcription {
namespace {

struct FormatDeleter {
  void operator()(AVFormatContext* value) const noexcept {
    avformat_close_input(&value);
  }
};
struct CodecDeleter {
  void operator()(AVCodecContext* value) const noexcept {
    avcodec_free_context(&value);
  }
};
struct FrameDeleter {
  void operator()(AVFrame* value) const noexcept {
    av_frame_free(&value);
  }
};
struct PacketDeleter {
  void operator()(AVPacket* value) const noexcept {
    av_packet_free(&value);
  }
};
struct SwrDeleter {
  void operator()(SwrContext* value) const noexcept {
    swr_free(&value);
  }
};
using Format = std::unique_ptr<AVFormatContext, FormatDeleter>;
using Codec = std::unique_ptr<AVCodecContext, CodecDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Swr = std::unique_ptr<SwrContext, SwrDeleter>;

[[nodiscard]] Error failure(ErrorCode code, std::string message, int native = 0) {
  return {.code = code, .native_code = native, .message = std::move(message), .retryable = false};
}

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

class FfmpegDecoder final : public AudioDecoder {
public:
  [[nodiscard]] Result<AudioData> decode(const std::filesystem::path& input,
                                         const AudioRange& range,
                                         const std::stop_token cancellation,
                                         const ProgressCallback& progress) override {
    if (cancellation.stop_requested()) {
      return Result<AudioData>::failure(failure(ErrorCode::Cancelled, "audio decode cancelled"));
    }
    if (range.start_centiseconds < 0 || range.duration_centiseconds < 0 ||
        (range.duration_centiseconds == 0 && range.start_centiseconds != 0) ||
        range.start_centiseconds > std::numeric_limits<std::int64_t>::max() / 160 ||
        range.duration_centiseconds > std::numeric_limits<std::int64_t>::max() / 160 ||
        range.start_centiseconds >
            std::numeric_limits<std::int64_t>::max() - range.duration_centiseconds) {
      return Result<AudioData>::failure(
          failure(ErrorCode::InvalidInput, "invalid audio transcription source range"));
    }
    AVFormatContext* raw_format = nullptr;
    const std::string path = utf8_path(input);
    int status = avformat_open_input(&raw_format, path.c_str(), nullptr, nullptr);
    if (status < 0 || raw_format == nullptr) {
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "cannot open audio input", status));
    }
    Format format(raw_format);
    status = avformat_find_stream_info(format.get(), nullptr);
    if (status < 0) {
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "cannot inspect audio input", status));
    }
    const int stream_index =
        av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "input has no audio stream", stream_index));
    }
    AVStream* stream = format->streams[stream_index];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "audio decoder is unavailable"));
    }
    Codec codec(avcodec_alloc_context3(decoder));
    if (!codec || avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0 ||
        avcodec_open2(codec.get(), decoder, nullptr) < 0) {
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "cannot initialize audio decoder"));
    }

    AVChannelLayout output_layout{};
    av_channel_layout_default(&output_layout, 1);
    SwrContext* raw_swr = nullptr;
    status =
        swr_alloc_set_opts2(&raw_swr, &output_layout, AV_SAMPLE_FMT_FLT, 16'000, &codec->ch_layout,
                            codec->sample_fmt, codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&output_layout);
    if (status < 0 || raw_swr == nullptr || swr_init(raw_swr) < 0) {
      if (raw_swr != nullptr)
        swr_free(&raw_swr);
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "cannot initialize audio resampler", status));
    }
    Swr swr(raw_swr);
    Frame frame(av_frame_alloc());
    Packet packet(av_packet_alloc());
    if (!frame || !packet) {
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "cannot allocate audio buffers"));
    }
    AudioData result;
    result.samples.reserve(range.is_full_input()
                               ? 16'000U * 30U
                               : static_cast<std::size_t>(range.duration_centiseconds) * 160U);
    const std::int64_t requested_start_sample = range.start_centiseconds * 160;
    const std::int64_t requested_end_sample =
        range.is_full_input() ? std::numeric_limits<std::int64_t>::max()
                              : requested_start_sample + range.duration_centiseconds * 160;
    const auto requested_sample_count =
        range.is_full_input() ? std::size_t{0}
                              : static_cast<std::size_t>(range.duration_centiseconds * 160);
    bool range_complete = false;
    const std::int64_t origin_timestamp =
        stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    const AVRational sample_timebase{1, 16'000};
    std::optional<std::int64_t> next_output_sample;
    std::optional<std::int64_t> last_timestamp;
    int last_input_samples = 0;
    std::int64_t seek_fallback_sample = 0;

    const auto append_converted = [&](const std::span<const float> converted,
                                      const std::int64_t frame_start_sample) -> Result<bool> {
      if (converted.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
          frame_start_sample > std::numeric_limits<std::int64_t>::max() -
                                   static_cast<std::int64_t>(converted.size())) {
        return Result<bool>::failure(
            failure(ErrorCode::AudioDecodeFailed, "decoded audio timestamp overflow"));
      }
      for (std::size_t sample = 0; sample < converted.size(); ++sample) {
        const std::int64_t absolute_sample = frame_start_sample + static_cast<std::int64_t>(sample);
        if (!range.is_full_input() &&
            (absolute_sample < requested_start_sample || absolute_sample >= requested_end_sample)) {
          continue;
        }
        if (result.samples.size() == result.samples.max_size()) {
          return Result<bool>::failure(
              failure(ErrorCode::AudioDecodeFailed, "decoded audio is too large"));
        }
        result.samples.push_back(converted[sample]);
        if (!range.is_full_input() && result.samples.size() == requested_sample_count) {
          range_complete = true;
          return Result<bool>::success(false);
        }
      }
      return Result<bool>::success(true);
    };

    // Seek to the nearest preceding key packet. Decoding starts before the
    // requested point and the exact sample window below removes the seek
    // preroll; this is required for compressed audio and non-zero stream PTS.
    if (!range.is_full_input()) {
      const std::int64_t start_us = range.start_centiseconds * 10'000;
      const std::int64_t seek_offset =
          av_rescale_q(start_us, AVRational{1, 1'000'000}, stream->time_base);
      const std::int64_t seek_target =
          seek_offset > std::numeric_limits<std::int64_t>::max() - origin_timestamp
              ? std::numeric_limits<std::int64_t>::max()
              : origin_timestamp + seek_offset;
      if (av_seek_frame(format.get(), stream_index, seek_target, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(codec.get());
        // Some elementary streams do not carry frame timestamps. For those,
        // the seek point is the only useful source position available.
        seek_fallback_sample = requested_start_sample;
      }
    }

    const auto receive = [&]() -> Result<bool> {
      while (true) {
        if (cancellation.stop_requested()) {
          return Result<bool>::failure(failure(ErrorCode::Cancelled, "audio decode cancelled"));
        }
        const int received = avcodec_receive_frame(codec.get(), frame.get());
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
          return Result<bool>::success(false);
        }
        if (received < 0) {
          return Result<bool>::failure(
              failure(ErrorCode::AudioDecodeFailed, "audio frame decode failed", received));
        }
        const std::int64_t capacity64 =
            av_rescale_rnd(swr_get_delay(swr.get(), codec->sample_rate) + frame->nb_samples, 16'000,
                           codec->sample_rate, AV_ROUND_UP);
        if (capacity64 <= 0 || capacity64 > std::numeric_limits<int>::max()) {
          return Result<bool>::failure(
              failure(ErrorCode::AudioDecodeFailed, "invalid resampler output size"));
        }
        const int capacity = static_cast<int>(capacity64);
        std::vector<float> converted(static_cast<std::size_t>(capacity));
        uint8_t* output[] = {reinterpret_cast<uint8_t*>(converted.data())};
        const int count =
            swr_convert(swr.get(), output, capacity,
                        const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        if (count < 0) {
          return Result<bool>::failure(
              failure(ErrorCode::AudioDecodeFailed, "audio resampling failed", count));
        }
        const bool has_timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE;
        const std::int64_t timestamp_start =
            has_timestamp ? av_rescale_q(frame->best_effort_timestamp - origin_timestamp,
                                         stream->time_base, sample_timebase)
                          : 0;
        std::int64_t frame_start_sample = 0;
        bool discontinuity = false;
        if (has_timestamp && last_timestamp.has_value()) {
          const auto timestamp_delta = timestamp_start - *last_timestamp;
          const auto expected_delta =
              av_rescale_q(last_input_samples, AVRational{1, codec->sample_rate}, sample_timebase);
          const auto timing_error = timestamp_delta - expected_delta;
          discontinuity = timing_error < -2 || timing_error > 2;
        }
        if (!next_output_sample.has_value()) {
          frame_start_sample = has_timestamp ? timestamp_start : seek_fallback_sample;
        } else if (!has_timestamp) {
          frame_start_sample = *next_output_sample;
        } else if (!discontinuity) {
          // Resampling distributes fractional samples differently from the
          // packet PTS rounding. Continuous frames therefore advance from
          // the previous converted output, not independently from each PTS.
          frame_start_sample = *next_output_sample;
        } else {
          frame_start_sample = timestamp_start;
        }
        if (frame_start_sample <= std::numeric_limits<std::int64_t>::max() - count)
          next_output_sample = frame_start_sample + count;
        if (has_timestamp) {
          last_timestamp = timestamp_start;
          last_input_samples = frame->nb_samples;
        } else {
          last_timestamp.reset();
          last_input_samples = 0;
        }
        const auto appended = append_converted(
            std::span<const float>(converted.data(), static_cast<std::size_t>(count)),
            frame_start_sample);
        if (!appended) {
          return appended;
        }
        if (!appended.value()) {
          av_frame_unref(frame.get());
          return Result<bool>::success(false);
        }
        av_frame_unref(frame.get());
      }
    };
    std::uint64_t packets = 0;
    while ((status = av_read_frame(format.get(), packet.get())) >= 0) {
      if (cancellation.stop_requested()) {
        return Result<AudioData>::failure(failure(ErrorCode::Cancelled, "audio decode cancelled"));
      }
      if (packet->stream_index == stream_index) {
        status = avcodec_send_packet(codec.get(), packet.get());
        if (status == AVERROR(EAGAIN)) {
          auto drained = receive();
          if (!drained)
            return Result<AudioData>::failure(drained.error());
          if (range_complete) {
            av_packet_unref(packet.get());
            break;
          }
          status = avcodec_send_packet(codec.get(), packet.get());
        }
        if (status < 0) {
          return Result<AudioData>::failure(
              failure(ErrorCode::AudioDecodeFailed, "cannot submit audio packet", status));
        }
        auto received = receive();
        if (!received)
          return Result<AudioData>::failure(received.error());
        if (range_complete)
          break;
      }
      av_packet_unref(packet.get());
      ++packets;
      if (progress && packets % 16U == 0U)
        progress(0.05, "decoding");
    }
    if (status < 0 && status != AVERROR_EOF) {
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "cannot read audio packets", status));
    }
    // A range completed before EOF needs no more packets or delayed samples.
    // At EOF, however, delayed decoder/resampler output can still belong to
    // the requested tail and must be drained through the same exact filter.
    if (!range.is_full_input() && range_complete) {
      if (progress)
        progress(0.15, "decoded");
      return Result<AudioData>::success(std::move(result));
    }
    status = avcodec_send_packet(codec.get(), nullptr);
    if (status == AVERROR(EAGAIN)) {
      auto drained = receive();
      if (!drained)
        return Result<AudioData>::failure(drained.error());
      status = avcodec_send_packet(codec.get(), nullptr);
    }
    if (status < 0 && status != AVERROR_EOF) {
      return Result<AudioData>::failure(
          failure(ErrorCode::AudioDecodeFailed, "cannot flush audio decoder", status));
    }
    while (true) {
      auto received = receive();
      if (!received)
        return Result<AudioData>::failure(received.error());
      if (!received.value())
        break;
    }
    while (swr_get_delay(swr.get(), codec->sample_rate) > 0) {
      const std::int64_t capacity64 = av_rescale_rnd(swr_get_delay(swr.get(), codec->sample_rate),
                                                     16'000, codec->sample_rate, AV_ROUND_UP);
      if (capacity64 <= 0 || capacity64 > std::numeric_limits<int>::max())
        break;
      const int capacity = static_cast<int>(capacity64);
      std::vector<float> converted(static_cast<std::size_t>(capacity));
      uint8_t* output[] = {reinterpret_cast<uint8_t*>(converted.data())};
      const int count = swr_convert(swr.get(), output, capacity, nullptr, 0);
      if (count <= 0)
        break;
      const std::int64_t flush_start = next_output_sample.value_or(0);
      const auto appended = append_converted(
          std::span<const float>(converted.data(), static_cast<std::size_t>(count)), flush_start);
      if (!appended)
        return Result<AudioData>::failure(appended.error());
      if (flush_start <= std::numeric_limits<std::int64_t>::max() - count)
        next_output_sample = flush_start + count;
      if (!appended.value())
        break;
    }
    if (progress)
      progress(0.15, "decoded");
    return Result<AudioData>::success(std::move(result));
  }
};

} // namespace

std::unique_ptr<AudioDecoder> make_ffmpeg_audio_decoder() {
  return std::make_unique<FfmpegDecoder>();
}

} // namespace video_editor::transcription
