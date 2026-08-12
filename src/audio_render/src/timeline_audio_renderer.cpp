// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/audio_render/track_dsp_chain.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace video_editor::audio_render {
namespace {

using Result = AudioRenderResult;

struct StereoSample final {
  float left{0.0F};
  float right{0.0F};
};

struct DecodedSamples final {
  std::unordered_map<std::int64_t, StereoSample> samples;
};

struct FormatCloser final {
  void operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
      avformat_close_input(&context);
    }
  }
};

struct CodecCloser final {
  void operator()(AVCodecContext* context) const noexcept {
    avcodec_free_context(&context);
  }
};

struct PacketCloser final {
  void operator()(AVPacket* packet) const noexcept {
    av_packet_free(&packet);
  }
};

struct FrameCloser final {
  void operator()(AVFrame* frame) const noexcept {
    av_frame_free(&frame);
  }
};

struct SwrCloser final {
  void operator()(SwrContext* context) const noexcept {
    swr_free(&context);
  }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using SwrPtr = std::unique_ptr<SwrContext, SwrCloser>;

[[nodiscard]] AudioRenderError make_error(const AudioRenderErrorCode code, std::string message) {
  return {.code = code,
          .message = std::move(message),
          .asset_id = std::nullopt,
          .clip_id = std::nullopt};
}

[[nodiscard]] std::string ffmpeg_error(const int value) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
  if (av_strerror(value, message.data(), message.size()) < 0) {
    return "FFmpeg error " + std::to_string(value);
  }
  return std::string(message.data());
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto encoded = path.u8string();
  return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
#else
  return path.string();
#endif
}

struct InterruptState final {
  const std::stop_token* token{nullptr};
};

extern "C" int interrupt_requested(void* opaque) noexcept {
  const auto* state = static_cast<const InterruptState*>(opaque);
  return state != nullptr && state->token != nullptr && state->token->stop_requested() ? 1 : 0;
}

[[nodiscard]] bool is_cancelled(const std::stop_token& token) noexcept {
  return token.stop_requested();
}

[[nodiscard]] std::int64_t source_sample_at(const edit::Clip& clip,
                                            const std::int64_t timeline_sample) {
  const edit::Time timeline_time(timeline_sample, kTimelineAudioSampleRate);
  const edit::Time timeline_offset = timeline_time - clip.timeline_range.start;
  const edit::Time source_offset = timeline_offset.scaled(
      clip.playback_rate.numerator(), clip.playback_rate.denominator(), edit::RoundingMode::Floor);
  edit::Time source_time;
  if (clip.reversed) {
    source_time = clip.source_range.end() - source_offset - edit::Time(1, kTimelineAudioSampleRate);
  } else {
    source_time = clip.source_range.start + source_offset;
  }
  if (!clip.source_range.contains(source_time)) {
    return -1;
  }
  return source_time.rescaledTo(kTimelineAudioSampleRate, edit::RoundingMode::Floor).value();
}

[[nodiscard]] double time_ratio(const edit::Time numerator, const edit::Time denominator) noexcept {
  if (denominator.isZero()) {
    return 1.0;
  }
  const long double top = static_cast<long double>(numerator.value()) *
                          static_cast<long double>(denominator.timescale());
  const long double bottom = static_cast<long double>(numerator.timescale()) *
                             static_cast<long double>(denominator.value());
  return static_cast<double>(top / bottom);
}

[[nodiscard]] float fade_gain(const edit::Clip& clip, const edit::Time timeline_time) noexcept {
  double factor = 1.0;
  const edit::Time offset = timeline_time - clip.timeline_range.start;
  if (!clip.fade_in.isZero() && offset < clip.fade_in) {
    factor = std::min(factor, std::clamp(time_ratio(offset, clip.fade_in), 0.0, 1.0));
  }
  const edit::Time remaining = clip.timeline_range.end() - timeline_time;
  if (!clip.fade_out.isZero() && remaining <= clip.fade_out) {
    factor = std::min(factor, std::clamp(time_ratio(remaining, clip.fade_out), 0.0, 1.0));
  }
  return static_cast<float>(factor);
}

[[nodiscard]] std::pair<std::size_t, std::size_t>
clip_output_bounds(const edit::Clip& clip, const AudioRenderRequest& request) {
  const edit::Time request_start(request.start_sample, kTimelineAudioSampleRate);
  const edit::Time request_duration(static_cast<std::int64_t>(request.sample_count),
                                    kTimelineAudioSampleRate);
  const edit::TimeRange overlap =
      clip.timeline_range.intersection(edit::TimeRange(request_start, request_duration));
  if (overlap.empty()) {
    return {0U, 0U};
  }

  const edit::Time relative_start = overlap.start - request_start;
  const edit::Time relative_end = overlap.end() - request_start;
  const std::int64_t first =
      relative_start.rescaledTo(kTimelineAudioSampleRate, edit::RoundingMode::Ceil).value();
  const std::int64_t last =
      relative_end.rescaledTo(kTimelineAudioSampleRate, edit::RoundingMode::Ceil).value();
  const auto begin = static_cast<std::size_t>(std::max<std::int64_t>(first, 0));
  const auto end = static_cast<std::size_t>(std::max<std::int64_t>(last, 0));
  return {std::min(begin, request.sample_count), std::min(end, request.sample_count)};
}

[[nodiscard]] edit::Result<DecodedSamples, AudioRenderError>
decode_requested_samples(const OriginalAudioMedia& media,
                         std::span<const std::int64_t> requested_samples,
                         const std::stop_token cancellation) {
  if (is_cancelled(cancellation)) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(
        make_error(AudioRenderErrorCode::Cancelled, "audio render was cancelled"));
  }
  if (requested_samples.empty()) {
    return edit::Result<DecodedSamples, AudioRenderError>::success({});
  }

  std::error_code file_error;
  if (!std::filesystem::is_regular_file(media.path, file_error) || file_error) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(
        make_error(AudioRenderErrorCode::MissingMedia,
                   "original audio media is missing: " + path_utf8(media.path)));
  }

  InterruptState interrupt{.token = &cancellation};
  AVFormatContext* raw_format = avformat_alloc_context();
  if (raw_format == nullptr) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(make_error(
        AudioRenderErrorCode::CannotOpenMedia, "could not allocate an FFmpeg format context"));
  }
  raw_format->interrupt_callback = {.callback = interrupt_requested, .opaque = &interrupt};

  const std::string encoded_path = path_utf8(media.path);
  int status = avformat_open_input(&raw_format, encoded_path.c_str(), nullptr, nullptr);
  if (status < 0) {
    if (raw_format != nullptr) {
      avformat_free_context(raw_format);
    }
    const auto code = is_cancelled(cancellation) ? AudioRenderErrorCode::Cancelled
                                                 : AudioRenderErrorCode::CannotOpenMedia;
    return edit::Result<DecodedSamples, AudioRenderError>::failure(
        make_error(code, "could not open original audio media: " + ffmpeg_error(status)));
  }
  FormatPtr format(raw_format);

  status = avformat_find_stream_info(format.get(), nullptr);
  if (status < 0) {
    const auto code = is_cancelled(cancellation) ? AudioRenderErrorCode::Cancelled
                                                 : AudioRenderErrorCode::CannotOpenMedia;
    return edit::Result<DecodedSamples, AudioRenderError>::failure(
        make_error(code, "could not read audio stream information: " + ffmpeg_error(status)));
  }

  int stream_index = media.audio_stream_index;
  if (stream_index < 0) {
    stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  }
  if (stream_index < 0 || static_cast<unsigned int>(stream_index) >= format->nb_streams ||
      format->streams[stream_index]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(make_error(
        AudioRenderErrorCode::AudioStreamNotFound, "original media has no matching audio stream"));
  }

  AVStream* const stream = format->streams[stream_index];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(make_error(
        AudioRenderErrorCode::DecoderUnavailable, "no decoder is available for the audio stream"));
  }
  CodecPtr decoder(avcodec_alloc_context3(codec));
  if (!decoder) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(make_error(
        AudioRenderErrorCode::DecoderUnavailable, "could not allocate the audio decoder"));
  }
  status = avcodec_parameters_to_context(decoder.get(), stream->codecpar);
  if (status < 0 || (status = avcodec_open2(decoder.get(), codec, nullptr)) < 0) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(
        make_error(AudioRenderErrorCode::DecoderUnavailable,
                   "could not initialize the audio decoder: " + ffmpeg_error(status)));
  }
  if (decoder->sample_rate <= 0 || decoder->ch_layout.nb_channels <= 0) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(make_error(
        AudioRenderErrorCode::DecodeFailed, "audio stream has an invalid sample format"));
  }

  AVChannelLayout output_layout{};
  av_channel_layout_default(&output_layout, static_cast<int>(kTimelineAudioChannels));
  SwrContext* raw_resampler = nullptr;
  status = swr_alloc_set_opts2(&raw_resampler, &output_layout, AV_SAMPLE_FMT_FLTP,
                               static_cast<int>(kTimelineAudioSampleRate), &decoder->ch_layout,
                               decoder->sample_fmt, decoder->sample_rate, 0, nullptr);
  av_channel_layout_uninit(&output_layout);
  if (status < 0 || raw_resampler == nullptr || swr_init(raw_resampler) < 0) {
    if (raw_resampler != nullptr) {
      swr_free(&raw_resampler);
    }
    return edit::Result<DecodedSamples, AudioRenderError>::failure(make_error(
        AudioRenderErrorCode::ResampleFailed, "could not initialize the 48 kHz stereo resampler"));
  }
  SwrPtr resampler(raw_resampler);
  PacketPtr packet(av_packet_alloc());
  FramePtr frame(av_frame_alloc());
  if (!packet || !frame) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(
        make_error(AudioRenderErrorCode::DecodeFailed, "could not allocate FFmpeg decode buffers"));
  }

  std::vector<std::int64_t> wanted(requested_samples.begin(), requested_samples.end());
  std::sort(wanted.begin(), wanted.end());
  wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
  const std::int64_t minimum_sample = wanted.front();
  const std::int64_t maximum_sample = wanted.back();
  if (minimum_sample < 0) {
    return edit::Result<DecodedSamples, AudioRenderError>::failure(make_error(
        AudioRenderErrorCode::InvalidTimeline, "clip maps before the beginning of its source"));
  }

  const std::int64_t origin_pts = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
  if (stream->start_time != AV_NOPTS_VALUE && minimum_sample > kTimelineAudioSampleRate) {
    const std::int64_t preroll_sample = minimum_sample - kTimelineAudioSampleRate;
    const std::int64_t target =
        origin_pts + av_rescale_q(preroll_sample,
                                  AVRational{1, static_cast<int>(kTimelineAudioSampleRate)},
                                  stream->time_base);
    if (av_seek_frame(format.get(), stream_index, target, AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(decoder.get());
      swr_close(resampler.get());
      if (swr_init(resampler.get()) < 0) {
        return edit::Result<DecodedSamples, AudioRenderError>::failure(
            make_error(AudioRenderErrorCode::ResampleFailed,
                       "could not reset the audio resampler after seek"));
      }
    }
  }

  DecodedSamples decoded;
  decoded.samples.reserve(wanted.size());
  bool have_timeline_origin = stream->start_time != AV_NOPTS_VALUE;
  std::int64_t timeline_origin_pts = origin_pts;
  std::int64_t next_output_sample = 0;
  std::optional<std::int64_t> previous_input_end;
  bool beyond_requested_range = false;

  const auto capture_converted = [&](const std::span<const float> left,
                                     const std::span<const float> right) {
    const auto converted = static_cast<std::int64_t>(left.size());
    const std::int64_t converted_end = next_output_sample + converted;
    auto wanted_it = std::lower_bound(wanted.begin(), wanted.end(), next_output_sample);
    while (wanted_it != wanted.end() && *wanted_it < converted_end) {
      const auto index = static_cast<std::size_t>(*wanted_it - next_output_sample);
      decoded.samples.insert_or_assign(*wanted_it, StereoSample{left[index], right[index]});
      ++wanted_it;
    }
    next_output_sample = converted_end;
  };

  const auto receive_frames = [&]() -> std::optional<AudioRenderError> {
    while (true) {
      if (is_cancelled(cancellation)) {
        return make_error(AudioRenderErrorCode::Cancelled, "audio render was cancelled");
      }
      const int receive_status = avcodec_receive_frame(decoder.get(), frame.get());
      if (receive_status == AVERROR(EAGAIN) || receive_status == AVERROR_EOF) {
        return std::nullopt;
      }
      if (receive_status < 0) {
        return make_error(AudioRenderErrorCode::DecodeFailed,
                          "audio frame decode failed: " + ffmpeg_error(receive_status));
      }

      std::int64_t frame_pts = frame->best_effort_timestamp;
      if (frame_pts == AV_NOPTS_VALUE) {
        frame_pts = frame->pts;
      }
      if (!have_timeline_origin && frame_pts != AV_NOPTS_VALUE) {
        timeline_origin_pts = frame_pts;
        have_timeline_origin = true;
      }
      std::int64_t frame_start = next_output_sample;
      if (frame_pts != AV_NOPTS_VALUE && have_timeline_origin) {
        frame_start = av_rescale_q(frame_pts - timeline_origin_pts, stream->time_base,
                                   AVRational{1, static_cast<int>(kTimelineAudioSampleRate)});
      }

      if (previous_input_end.has_value()) {
        const std::int64_t discontinuity = frame_start - *previous_input_end;
        if (std::abs(discontinuity) > 2) {
          next_output_sample += discontinuity;
        }
      } else {
        next_output_sample = frame_start;
      }

      const std::int64_t output_capacity_64 = av_rescale_rnd(
          swr_get_delay(resampler.get(), decoder->sample_rate) + frame->nb_samples,
          static_cast<std::int64_t>(kTimelineAudioSampleRate), decoder->sample_rate, AV_ROUND_UP);
      if (output_capacity_64 <= 0 ||
          output_capacity_64 > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return make_error(AudioRenderErrorCode::ResampleFailed,
                          "resampler requested an invalid output buffer size");
      }
      const int output_capacity = static_cast<int>(output_capacity_64);
      std::vector<float> left(static_cast<std::size_t>(output_capacity));
      std::vector<float> right(static_cast<std::size_t>(output_capacity));
      std::array<std::uint8_t*, 2> output_planes{reinterpret_cast<std::uint8_t*>(left.data()),
                                                 reinterpret_cast<std::uint8_t*>(right.data())};
      const int converted =
          swr_convert(resampler.get(), output_planes.data(), output_capacity,
                      const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
      if (converted < 0) {
        return make_error(AudioRenderErrorCode::ResampleFailed,
                          "audio resampling failed: " + ffmpeg_error(converted));
      }

      capture_converted(std::span<const float>(left.data(), static_cast<std::size_t>(converted)),
                        std::span<const float>(right.data(), static_cast<std::size_t>(converted)));
      const std::int64_t input_duration =
          av_rescale_q(frame->nb_samples, AVRational{1, decoder->sample_rate},
                       AVRational{1, static_cast<int>(kTimelineAudioSampleRate)});
      previous_input_end = frame_start + input_duration;
      beyond_requested_range = next_output_sample > maximum_sample + kTimelineAudioSampleRate;
      av_frame_unref(frame.get());
      if (beyond_requested_range) {
        return std::nullopt;
      }
    }
  };

  while (!beyond_requested_range && (status = av_read_frame(format.get(), packet.get())) >= 0) {
    if (is_cancelled(cancellation)) {
      return edit::Result<DecodedSamples, AudioRenderError>::failure(
          make_error(AudioRenderErrorCode::Cancelled, "audio render was cancelled"));
    }
    if (packet->stream_index == stream_index) {
      status = avcodec_send_packet(decoder.get(), packet.get());
      if (status < 0 && status != AVERROR(EAGAIN)) {
        return edit::Result<DecodedSamples, AudioRenderError>::failure(
            make_error(AudioRenderErrorCode::DecodeFailed,
                       "could not submit an audio packet: " + ffmpeg_error(status)));
      }
      if (const auto issue = receive_frames()) {
        return edit::Result<DecodedSamples, AudioRenderError>::failure(*issue);
      }
    }
    av_packet_unref(packet.get());
  }
  if (!beyond_requested_range) {
    if (status != AVERROR_EOF && status < 0) {
      const auto code = is_cancelled(cancellation) ? AudioRenderErrorCode::Cancelled
                                                   : AudioRenderErrorCode::DecodeFailed;
      return edit::Result<DecodedSamples, AudioRenderError>::failure(
          make_error(code, "reading original audio failed: " + ffmpeg_error(status)));
    }
    status = avcodec_send_packet(decoder.get(), nullptr);
    if (status >= 0) {
      if (const auto issue = receive_frames()) {
        return edit::Result<DecodedSamples, AudioRenderError>::failure(*issue);
      }
    }

    while (swr_get_delay(resampler.get(), decoder->sample_rate) > 0) {
      const std::int64_t capacity_64 = av_rescale_rnd(
          swr_get_delay(resampler.get(), decoder->sample_rate),
          static_cast<std::int64_t>(kTimelineAudioSampleRate), decoder->sample_rate, AV_ROUND_UP);
      if (capacity_64 <= 0 ||
          capacity_64 > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return edit::Result<DecodedSamples, AudioRenderError>::failure(make_error(
            AudioRenderErrorCode::ResampleFailed, "resampler flush buffer size is invalid"));
      }
      const int capacity = static_cast<int>(capacity_64);
      std::vector<float> left(static_cast<std::size_t>(capacity));
      std::vector<float> right(static_cast<std::size_t>(capacity));
      std::array<std::uint8_t*, 2> planes{reinterpret_cast<std::uint8_t*>(left.data()),
                                          reinterpret_cast<std::uint8_t*>(right.data())};
      const int converted = swr_convert(resampler.get(), planes.data(), capacity, nullptr, 0);
      if (converted < 0) {
        return edit::Result<DecodedSamples, AudioRenderError>::failure(
            make_error(AudioRenderErrorCode::ResampleFailed,
                       "audio resampler flush failed: " + ffmpeg_error(converted)));
      }
      if (converted == 0) {
        break;
      }
      capture_converted(std::span<const float>(left.data(), static_cast<std::size_t>(converted)),
                        std::span<const float>(right.data(), static_cast<std::size_t>(converted)));
    }
  }

  return edit::Result<DecodedSamples, AudioRenderError>::success(std::move(decoded));
}

[[nodiscard]] Result request_error(const AudioRenderErrorCode code, std::string message) {
  return Result::failure(make_error(code, std::move(message)));
}

} // namespace

class TimelineAudioRenderer::Impl final {
public:
  explicit Impl(std::shared_ptr<const OriginalAudioProvider> source_provider)
      : originals(std::move(source_provider)) {}

  std::shared_ptr<const OriginalAudioProvider> originals;
};

TimelineAudioRenderer::TimelineAudioRenderer(std::shared_ptr<const OriginalAudioProvider> originals)
    : impl_(std::make_unique<Impl>(std::move(originals))) {}

TimelineAudioRenderer::~TimelineAudioRenderer() = default;

TimelineAudioRenderer::TimelineAudioRenderer(TimelineAudioRenderer&&) noexcept = default;

TimelineAudioRenderer& TimelineAudioRenderer::operator=(TimelineAudioRenderer&&) noexcept = default;

AudioRenderResult TimelineAudioRenderer::render(const edit::TimelineSnapshot& snapshot,
                                                const AudioRenderRequest& request) const {
  if (!impl_->originals) {
    return request_error(AudioRenderErrorCode::InvalidRequest,
                         "an original-media provider is required");
  }
  if (request.sample_count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
      request.start_sample > std::numeric_limits<std::int64_t>::max() -
                                 static_cast<std::int64_t>(request.sample_count)) {
    return request_error(AudioRenderErrorCode::InvalidRequest,
                         "requested audio sample range overflows int64");
  }
  if (is_cancelled(request.cancellation)) {
    return request_error(AudioRenderErrorCode::Cancelled, "audio render was cancelled");
  }

  audio::AudioBlock output(
      {.sample_rate = kTimelineAudioSampleRate, .channels = kTimelineAudioChannels},
      request.start_sample, request.sample_count);
  if (request.sample_count == 0U) {
    return Result::success(std::move(output));
  }

  const edit::Sequence& sequence = snapshot.sequence();
  if (sequence.audio_sample_rate != kTimelineAudioSampleRate) {
    return request_error(AudioRenderErrorCode::InvalidTimeline,
                         "beta timeline audio must use a 48 kHz master sample rate");
  }

  const bool has_solo =
      std::any_of(sequence.tracks.begin(), sequence.tracks.end(), [](const edit::Track& track) {
        return track.kind == edit::TrackKind::Audio && track.solo;
      });
  for (const edit::Track& track : sequence.tracks) {
    if (track.kind != edit::TrackKind::Audio || track.muted || (has_solo && !track.solo)) {
      continue;
    }
    // Render this track into a temporary block so track-level DSP (EQ,
    // compressor, denoise, limiter) can be applied to the isolated track mix
    // before summing into the master output. This preserves correct filter
    // state and prevents one track's DSP from affecting another.
    audio::AudioBlock track_output(
        {.sample_rate = kTimelineAudioSampleRate, .channels = kTimelineAudioChannels},
        request.start_sample, request.sample_count);
    bool track_has_audio = false;
    for (const edit::Clip& clip : track.clips) {
      if (clip.kind != edit::ClipKind::Audio) {
        continue;
      }
      const auto [begin, end] = clip_output_bounds(clip, request);
      if (begin >= end) {
        continue;
      }
      if (!std::isfinite(clip.audio_gain_db) || !std::isfinite(clip.audio_pan)) {
        AudioRenderError issue = make_error(AudioRenderErrorCode::InvalidTimeline,
                                            "clip has non-finite audio gain or pan");
        issue.clip_id = clip.id;
        issue.asset_id = clip.asset_id;
        return Result::failure(std::move(issue));
      }
      const edit::Asset* const asset = edit::findAsset(snapshot.project(), clip.asset_id);
      if (asset == nullptr || !asset->has_audio) {
        AudioRenderError issue = make_error(AudioRenderErrorCode::MissingAsset,
                                            "audio clip references a missing or non-audio asset");
        issue.clip_id = clip.id;
        issue.asset_id = clip.asset_id;
        return Result::failure(std::move(issue));
      }
      const auto media = impl_->originals->resolve_original(clip.asset_id);
      if (!media.has_value()) {
        AudioRenderError issue =
            make_error(AudioRenderErrorCode::MissingMedia,
                       "no authoritative original is registered for audio asset");
        issue.clip_id = clip.id;
        issue.asset_id = clip.asset_id;
        return Result::failure(std::move(issue));
      }

      std::vector<std::int64_t> source_positions(end - begin, -1);
      std::vector<std::int64_t> requested_sources;
      requested_sources.reserve(end - begin);
      for (std::size_t index = begin; index < end; ++index) {
        const std::int64_t absolute_sample =
            request.start_sample + static_cast<std::int64_t>(index);
        const std::int64_t source_sample = source_sample_at(clip, absolute_sample);
        source_positions[index - begin] = source_sample;
        if (source_sample >= 0) {
          requested_sources.push_back(source_sample);
        }
      }
      auto decoded = decode_requested_samples(*media, requested_sources, request.cancellation);
      if (!decoded) {
        AudioRenderError issue = decoded.error();
        issue.clip_id = clip.id;
        issue.asset_id = clip.asset_id;
        return Result::failure(std::move(issue));
      }

      const float clip_gain = static_cast<float>(std::pow(10.0, clip.audio_gain_db / 20.0));
      const float pan = static_cast<float>(std::clamp(clip.audio_pan, -1.0, 1.0));
      const float pan_angle = (pan + 1.0F) * (std::numbers::pi_v<float> / 4.0F);
      const float left_pan = std::cos(pan_angle);
      const float right_pan = std::sin(pan_angle);
      // Track-level gain/pan is a separate mixer stage. At defaults (0 dB,
      // pan 0) it must be unity so an unadjusted track is bit-identical to the
      // clip mix. Track pan therefore uses a linear law (pan=-1 -> left only,
      // pan 0 -> both unity, pan=+1 -> right only) rather than the equal-power
      // law used for clip pan, because a mixer fader's center detent should
      // not attenuate the signal.
      const float track_gain =
          static_cast<float>(std::pow(10.0, track.audio_gain_db / 20.0));
      const float track_pan =
          static_cast<float>(std::clamp(track.audio_pan, -1.0, 1.0));
      const float track_left_pan = std::clamp(1.0F - track_pan, 0.0F, 1.0F);
      const float track_right_pan = std::clamp(1.0F + track_pan, 0.0F, 1.0F);
      auto left_track = track_output.channel(0);
      auto right_track = track_output.channel(1);
      for (std::size_t index = begin; index < end; ++index) {
        if (is_cancelled(request.cancellation)) {
          return request_error(AudioRenderErrorCode::Cancelled, "audio render was cancelled");
        }
        const auto found = decoded.value().samples.find(source_positions[index - begin]);
        if (found == decoded.value().samples.end()) {
          continue;
        }
        const std::int64_t absolute_sample =
            request.start_sample + static_cast<std::int64_t>(index);
        const float envelope =
            fade_gain(clip, edit::Time(absolute_sample, kTimelineAudioSampleRate));
        const float left_sample =
            found->second.left * clip_gain * left_pan * envelope * track_gain * track_left_pan;
        const float right_sample =
            found->second.right * clip_gain * right_pan * envelope * track_gain * track_right_pan;
        left_track[index] += left_sample;
        right_track[index] += right_sample;
        track_has_audio = true;
      }
    }
    if (!track_has_audio) {
      continue;
    }
    // Apply track-level DSP chain (EQ, compressor, dialogue denoise, limiter)
    // to the isolated track mix before summing into the master output.
    if (!track.effects.empty()) {
      TrackDspChain dsp_chain;
      dsp_chain.configure(track.effects, static_cast<float>(kTimelineAudioSampleRate));
      if (!dsp_chain.empty()) {
        dsp_chain.process(track_output);
      }
    }
    // Sum the processed track into the master output.
    auto left_output = output.channel(0);
    auto right_output = output.channel(1);
    auto left_track = track_output.channel(0);
    auto right_track = track_output.channel(1);
    for (std::size_t index = 0; index < request.sample_count; ++index) {
      left_output[index] += left_track[index];
      right_output[index] += right_track[index];
    }
  }

  return Result::success(std::move(output));
}

} // namespace video_editor::audio_render
