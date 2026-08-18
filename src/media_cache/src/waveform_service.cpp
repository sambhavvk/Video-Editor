// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_cache/waveform_service.h"
#include "video_editor/media_codec/format_open.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace video_editor::media_cache {
namespace {

constexpr std::array<std::byte, 8> kWaveformMagic{std::byte{'V'}, std::byte{'E'}, std::byte{'W'},
                                                    std::byte{'A'}, std::byte{'V'}, std::byte{'E'},
                                                    std::byte{'0'}, std::byte{'1'}};

// ---------------------------------------------------------------------------
// Error helpers. The public API returns WaveformResult; internally we throw a
// small exception type and translate it back to a result at the boundary,
// mirroring the proxy_service ProxyFailure pattern.
// ---------------------------------------------------------------------------
class WaveformFailure final : public std::exception {
public:
  explicit WaveformFailure(WaveformError error) : error_(std::move(error)) {}
  [[nodiscard]] const WaveformError& error() const noexcept { return error_; }

private:
  WaveformError error_;
};

[[noreturn]] void fail(WaveformErrorCode code, std::string message, const int native_code = 0) {
  throw WaveformFailure({.code = code, .native_code = native_code, .message = std::move(message)});
}

[[nodiscard]] std::string ffmpeg_error(const int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error";
  }
  return std::string(buffer.data());
}

void require_ffmpeg(const int code, const WaveformErrorCode error_code,
                    const std::string_view action) {
  if (code < 0) {
    fail(error_code, std::string(action) + ": " + ffmpeg_error(code), code);
  }
}

// ---------------------------------------------------------------------------
// FFmpeg RAII deleters, matching src/proxy_service/src/proxy_service.cpp.
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

struct SwrDeleter {
  void operator()(SwrContext* context) const noexcept {
    swr_free(&context);
  }
};

using InputFormat = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using CodecContext = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using ResampleContext = std::unique_ptr<SwrContext, SwrDeleter>;

[[nodiscard]] std::string native_path(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
#else
  return path.string();
#endif
}

[[nodiscard]] Frame make_frame() {
  Frame frame(av_frame_alloc());
  if (!frame) {
    fail(WaveformErrorCode::Internal, "cannot allocate an FFmpeg frame", AVERROR(ENOMEM));
  }
  return frame;
}

[[nodiscard]] Packet make_packet() {
  Packet packet(av_packet_alloc());
  if (!packet) {
    fail(WaveformErrorCode::Internal, "cannot allocate an FFmpeg packet", AVERROR(ENOMEM));
  }
  return packet;
}

void check_cancelled(const std::stop_token cancellation) {
  if (cancellation.stop_requested()) {
    fail(WaveformErrorCode::Cancelled, "waveform generation was cancelled", AVERROR_EXIT);
  }
}

struct InterruptState {
  std::stop_token cancellation;
};

int interrupt_callback(void* opaque) noexcept {
  const auto* state = static_cast<const InterruptState*>(opaque);
  return state != nullptr && state->cancellation.stop_requested() ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Mono mixdown. The decode path may produce multi-channel interleaved float
// samples when WaveformOptions::channel_count > 1; the pyramid is always
// mono, so we average the channels here.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<float> mix_to_mono(const std::vector<float>& interleaved,
                                             const std::int64_t channel_count) {
  if (channel_count <= 1) {
    return interleaved;
  }
  const std::int64_t frames =
      static_cast<std::int64_t>(interleaved.size()) / channel_count;
  std::vector<float> mono(static_cast<std::size_t>(frames));
  const double norm = 1.0 / static_cast<double>(channel_count);
  for (std::int64_t f = 0; f < frames; ++f) {
    double sum = 0.0;
    for (std::int64_t c = 0; c < channel_count; ++c) {
      sum += static_cast<double>(interleaved[static_cast<std::size_t>(f * channel_count + c)]);
    }
    mono[static_cast<std::size_t>(f)] = static_cast<float>(sum * norm);
  }
  return mono;
}

// ---------------------------------------------------------------------------
// Little-endian serialization helpers, mirroring proxy_service.
// ---------------------------------------------------------------------------
template <typename Integer>
void append_little_endian(std::vector<std::byte>& output, const Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  const Unsigned bits = static_cast<Unsigned>(value);
  for (std::size_t shift = 0; shift < sizeof(Integer); ++shift) {
    output.push_back(
        static_cast<std::byte>((bits >> static_cast<Unsigned>(shift * 8U)) & static_cast<Unsigned>(0xFFU)));
  }
}

void append_f32(std::vector<std::byte>& output, const float value) {
  const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
  append_little_endian(output, bits);
}

class ByteReader final {
public:
  explicit ByteReader(const std::span<const std::byte> bytes) : remaining_(bytes) {}

  template <typename Integer> [[nodiscard]] Integer integer() {
    if (remaining_.size() < sizeof(Integer)) {
      fail(WaveformErrorCode::InvalidArgument, "waveform blob is truncated");
    }
    using Unsigned = std::make_unsigned_t<Integer>;
    std::uintmax_t value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
      value |= static_cast<std::uintmax_t>(std::to_integer<unsigned char>(remaining_[index]))
               << static_cast<std::uintmax_t>(index * 8U);
    }
    remaining_ = remaining_.subspan(sizeof(Integer));
    return static_cast<Integer>(static_cast<Unsigned>(value));
  }

  [[nodiscard]] float f32() {
    const std::uint32_t bits = integer<std::uint32_t>();
    return std::bit_cast<float>(bits);
  }

  void magic() {
    if (remaining_.size() < kWaveformMagic.size() ||
        !std::equal(kWaveformMagic.begin(), kWaveformMagic.end(), remaining_.begin())) {
      fail(WaveformErrorCode::InvalidArgument, "blob is not a Video Editor waveform");
    }
    remaining_ = remaining_.subspan(kWaveformMagic.size());
  }

  [[nodiscard]] std::size_t remaining() const noexcept { return remaining_.size(); }

private:
  std::span<const std::byte> remaining_;
};

} // namespace

// ---------------------------------------------------------------------------
// Pure functions.
// ---------------------------------------------------------------------------

std::vector<std::int64_t>
waveform_level_bucket_counts(const std::int64_t finest_level_buckets, const int level_count) noexcept {
  std::vector<std::int64_t> result;
  if (level_count <= 0 || finest_level_buckets < 2) {
    return result;
  }
  result.reserve(static_cast<std::size_t>(level_count));
  std::int64_t current = finest_level_buckets;
  for (int index = 0; index < level_count; ++index) {
    if (index > 0) {
      current /= 2;
      if (current < 2) {
        break;
      }
    }
    result.push_back(current);
  }
  return result;
}

std::string waveform_parameter_hash(const WaveformOptions& options) {
  return "b" + std::to_string(options.finest_level_buckets) + "l" +
         std::to_string(options.level_count) + "r" + std::to_string(options.sample_rate) + "c" +
         std::to_string(options.channel_count);
}

WaveformLevel build_waveform_level(const std::vector<float>& samples,
                                   const std::int64_t sample_rate,
                                   const std::int64_t channel_count,
                                   const std::int64_t bucket_count) {
  WaveformLevel level;
  level.sample_rate = sample_rate;
  level.channel_count = 1; // buckets always describe mono-mixed data
  level.bucket_count = bucket_count;

  if (bucket_count <= 0) {
    return level;
  }

  // Mix interleaved multi-channel input down to mono.
  std::vector<float> mono;
  if (channel_count <= 1) {
    mono = samples;
  } else {
    const std::int64_t frames =
        static_cast<std::int64_t>(samples.size()) / channel_count;
    mono.resize(static_cast<std::size_t>(frames));
    const double norm = 1.0 / static_cast<double>(channel_count);
    for (std::int64_t f = 0; f < frames; ++f) {
      double sum = 0.0;
      for (std::int64_t c = 0; c < channel_count; ++c) {
        sum += static_cast<double>(samples[static_cast<std::size_t>(f * channel_count + c)]);
      }
      mono[static_cast<std::size_t>(f)] = static_cast<float>(sum * norm);
    }
  }

  const std::int64_t sample_count = static_cast<std::int64_t>(mono.size());
  level.sample_count = sample_count;
  const std::int64_t samples_per_bucket =
      std::max<std::int64_t>(1, sample_count / bucket_count);

  level.buckets.reserve(static_cast<std::size_t>(bucket_count));
  for (std::int64_t b = 0; b < bucket_count; ++b) {
    const std::int64_t start = b * samples_per_bucket;
    const std::int64_t end = std::min(start + samples_per_bucket, sample_count);
    if (start >= sample_count || end <= start) {
      // Sentinel "no data": min>max signals an empty bucket to the UI.
      level.buckets.push_back({.minimum = 1.0f, .maximum = -1.0f, .rms = 0.0f});
      continue;
    }
    float minimum = mono[static_cast<std::size_t>(start)];
    float maximum = minimum;
    double sum_sq = static_cast<double>(minimum) * static_cast<double>(minimum);
    std::int64_t n = 1;
    for (std::int64_t i = start + 1; i < end; ++i) {
      const float s = mono[static_cast<std::size_t>(i)];
      if (s < minimum) {
        minimum = s;
      }
      if (s > maximum) {
        maximum = s;
      }
      const double ds = static_cast<double>(s);
      sum_sq += ds * ds;
      ++n;
    }
    minimum = std::clamp(minimum, -1.0f, 1.0f);
    maximum = std::clamp(maximum, -1.0f, 1.0f);
    const float rms =
        n > 0 ? static_cast<float>(std::sqrt(sum_sq / static_cast<double>(n))) : 0.0f;
    level.buckets.push_back({.minimum = minimum, .maximum = maximum, .rms = rms});
  }
  return level;
}

Waveform build_waveform_pyramid(const std::vector<float>& mono_samples,
                               const std::int64_t sample_rate,
                               const WaveformOptions& options) {
  Waveform waveform;
  waveform.source_stream_index = -1;
  waveform.sample_rate = options.sample_rate;
  waveform.channel_count = 1;
  waveform.total_samples = static_cast<std::int64_t>(mono_samples.size());

  const std::vector<std::int64_t> counts =
      waveform_level_bucket_counts(options.finest_level_buckets, options.level_count);
  waveform.levels.reserve(counts.size());
  for (const std::int64_t bucket_count : counts) {
    waveform.levels.push_back(
        build_waveform_level(mono_samples, sample_rate, 1, bucket_count));
  }
  return waveform;
}

std::vector<std::byte> serialize_waveform(const Waveform& waveform) {
  std::vector<std::byte> output;
  output.reserve(64U + waveform.levels.size() * 32U);
  output.insert(output.end(), kWaveformMagic.begin(), kWaveformMagic.end());
  append_little_endian(output, waveform.sample_rate);
  append_little_endian(output, waveform.channel_count);
  append_little_endian(output, waveform.total_samples);
  append_little_endian(output, waveform.source_stream_index);
  if (waveform.levels.size() > std::numeric_limits<std::int32_t>::max()) {
    fail(WaveformErrorCode::InvalidArgument, "waveform has too many levels");
  }
  append_little_endian(output, static_cast<std::int32_t>(waveform.levels.size()));
  for (const WaveformLevel& level : waveform.levels) {
    append_little_endian(output, level.bucket_count);
    append_little_endian(output, level.sample_count);
    append_little_endian(output, level.sample_rate);
    append_little_endian(output, level.channel_count);
    for (const WaveformBucket& bucket : level.buckets) {
      append_f32(output, bucket.minimum);
      append_f32(output, bucket.maximum);
      append_f32(output, bucket.rms);
    }
  }
  return output;
}

WaveformResult<Waveform> deserialize_waveform(const std::span<const std::byte> bytes) {
  try {
    ByteReader reader(bytes);
    reader.magic();

    Waveform waveform;
    waveform.sample_rate = reader.integer<std::int64_t>();
    waveform.channel_count = reader.integer<std::int64_t>();
    waveform.total_samples = reader.integer<std::int64_t>();
    waveform.source_stream_index = reader.integer<std::int64_t>();
    if (waveform.sample_rate < 0 || waveform.channel_count < 0 || waveform.total_samples < 0) {
      fail(WaveformErrorCode::InvalidArgument, "waveform blob contains a negative count");
    }

    const std::int32_t level_count = reader.integer<std::int32_t>();
    if (level_count < 0) {
      fail(WaveformErrorCode::InvalidArgument, "waveform blob has a negative level count");
    }
    // Each level header is 4 * 8 = 32 bytes; guard against implausible counts
    // before allocating.
    if (static_cast<std::uintmax_t>(level_count) > static_cast<std::uintmax_t>(reader.remaining()) / 32U) {
      fail(WaveformErrorCode::InvalidArgument, "waveform level count exceeds its payload");
    }
    waveform.levels.reserve(static_cast<std::size_t>(level_count));

    for (std::int32_t l = 0; l < level_count; ++l) {
      WaveformLevel level;
      level.bucket_count = reader.integer<std::int64_t>();
      level.sample_count = reader.integer<std::int64_t>();
      level.sample_rate = reader.integer<std::int64_t>();
      level.channel_count = reader.integer<std::int64_t>();
      if (level.bucket_count < 0 || level.sample_count < 0 || level.sample_rate < 0 ||
          level.channel_count < 0) {
        fail(WaveformErrorCode::InvalidArgument, "waveform level contains a negative count");
      }
      // Each bucket is 3 * 4 = 12 bytes.
      if (static_cast<std::uintmax_t>(level.bucket_count) >
          static_cast<std::uintmax_t>(reader.remaining()) / 12U) {
        fail(WaveformErrorCode::InvalidArgument, "waveform bucket count exceeds its payload");
      }
      level.buckets.reserve(static_cast<std::size_t>(level.bucket_count));
      for (std::int64_t b = 0; b < level.bucket_count; ++b) {
        WaveformBucket bucket;
        bucket.minimum = reader.f32();
        bucket.maximum = reader.f32();
        bucket.rms = reader.f32();
        if (std::isnan(bucket.minimum) || std::isnan(bucket.maximum) || std::isnan(bucket.rms)) {
          fail(WaveformErrorCode::InvalidArgument, "waveform bucket contains a NaN");
        }
        const bool sentinel = bucket.minimum == 1.0f && bucket.maximum == -1.0f;
        if (!sentinel && bucket.minimum > bucket.maximum) {
          fail(WaveformErrorCode::InvalidArgument, "waveform bucket has min greater than max");
        }
        level.buckets.push_back(bucket);
      }
      waveform.levels.push_back(std::move(level));
    }

    if (reader.remaining() != 0) {
      fail(WaveformErrorCode::InvalidArgument, "waveform blob has trailing bytes");
    }
    return WaveformResult<Waveform>::success(std::move(waveform));
  } catch (const WaveformFailure& failure) {
    return WaveformResult<Waveform>::failure(failure.error());
  }
}

// ---------------------------------------------------------------------------
// Cache-backed load.
// ---------------------------------------------------------------------------

WaveformResult<Waveform> load_waveform(const std::string& asset_id,
                                       const WaveformOptions& options,
                                       CacheStore& cache) {
  const CacheKey key{.asset_id = asset_id,
                    .kind = CacheKind::Waveform,
                    .parameter_hash = waveform_parameter_hash(options)};
  CacheResult<std::vector<std::byte>> got = cache.get(key);
  if (!got.has_value()) {
    if (got.error().code == CacheErrorCode::NotFound) {
      return WaveformResult<Waveform>::failure(
          {.code = WaveformErrorCode::NotFound,
           .native_code = got.error().native_code,
           .message = got.error().message});
    }
    return WaveformResult<Waveform>::failure(
        {.code = WaveformErrorCode::StoreFailed,
         .native_code = got.error().native_code,
         .message = got.error().message});
  }
  WaveformResult<Waveform> parsed = deserialize_waveform(got.value());
  return parsed;
}

// ---------------------------------------------------------------------------
// FFmpeg decode + resample path.
// ---------------------------------------------------------------------------

namespace {

// Appends resampled interleaved float samples from a decoded frame into the
// accumulator. The resampler target is AV_SAMPLE_FMT_FLT at the requested
// sample rate and channel count.
void append_resampled(const AVFrame* source, const CodecContext& decoder,
                      const ResampleContext& resampler, const AVChannelLayout& out_layout,
                      const std::int64_t out_sample_rate, const std::int64_t out_channels,
                      std::vector<float>& samples) {
  const int output_capacity = static_cast<int>(av_rescale_rnd(
      swr_get_delay(resampler.get(), decoder->sample_rate) + source->nb_samples,
      static_cast<int>(out_sample_rate), decoder->sample_rate, AV_ROUND_UP));
  if (output_capacity <= 0) {
    return;
  }
  Frame output = make_frame();
  output->format = AV_SAMPLE_FMT_FLT;
  output->sample_rate = static_cast<int>(out_sample_rate);
  output->nb_samples = output_capacity;
  require_ffmpeg(av_channel_layout_copy(&output->ch_layout, &out_layout),
                 WaveformErrorCode::Internal, "copy waveform output channel layout");
  require_ffmpeg(av_frame_get_buffer(output.get(), 0), WaveformErrorCode::Internal,
                 "allocate waveform output frame");

  const int input_plane_count =
      av_sample_fmt_is_planar(static_cast<AVSampleFormat>(source->format)) != 0
          ? source->ch_layout.nb_channels
          : 1;
  if (input_plane_count <= 0) {
    fail(WaveformErrorCode::DecodeFailed, "decoded audio frame has no channels");
  }
  std::vector<const std::uint8_t*> input_planes(static_cast<std::size_t>(input_plane_count));
  std::copy_n(source->extended_data, input_plane_count, input_planes.begin());

  const int converted = swr_convert(resampler.get(), output->extended_data, output_capacity,
                                    input_planes.data(), source->nb_samples);
  require_ffmpeg(converted, WaveformErrorCode::ResampleFailed, "resample waveform frame");
  if (converted <= 0) {
    return;
  }
  // AV_SAMPLE_FMT_FLT is interleaved: a single plane holds converted * channels
  // floats.
  const float* data = reinterpret_cast<const float*>(output->extended_data[0]);
  const std::size_t count =
      static_cast<std::size_t>(converted) * static_cast<std::size_t>(out_channels);
  samples.insert(samples.end(), data, data + count);
}

void flush_resampler(const CodecContext& decoder, const ResampleContext& resampler,
                     const AVChannelLayout& out_layout, const std::int64_t out_sample_rate,
                     const std::int64_t out_channels, std::vector<float>& samples) {
  for (;;) {
    const std::int64_t delay = swr_get_delay(resampler.get(), decoder->sample_rate);
    if (delay <= 0) {
      break;
    }
    const int output_capacity = static_cast<int>(av_rescale_rnd(
        delay, static_cast<int>(out_sample_rate), decoder->sample_rate, AV_ROUND_UP));
    if (output_capacity <= 0) {
      break;
    }
    Frame output = make_frame();
    output->format = AV_SAMPLE_FMT_FLT;
    output->sample_rate = static_cast<int>(out_sample_rate);
    output->nb_samples = output_capacity;
    require_ffmpeg(av_channel_layout_copy(&output->ch_layout, &out_layout),
                   WaveformErrorCode::Internal, "copy flushed waveform channel layout");
    require_ffmpeg(av_frame_get_buffer(output.get(), 0), WaveformErrorCode::Internal,
                   "allocate flushed waveform frame");
    const int converted =
        swr_convert(resampler.get(), output->extended_data, output_capacity, nullptr, 0);
    require_ffmpeg(converted, WaveformErrorCode::ResampleFailed, "flush waveform resampler");
    if (converted <= 0) {
      break;
    }
    const float* data = reinterpret_cast<const float*>(output->extended_data[0]);
    const std::size_t count =
        static_cast<std::size_t>(converted) * static_cast<std::size_t>(out_channels);
    samples.insert(samples.end(), data, data + count);
  }
}

} // namespace

WaveformResult<Waveform> generate_waveform(const std::filesystem::path& asset_uri,
                                            const int stream_index,
                                            const WaveformOptions& options,
                                            const std::string& asset_id,
                                            CacheStore& cache,
                                            std::stop_token cancellation) {
  try {
    if (options.finest_level_buckets < 2 || options.level_count < 1 ||
        options.sample_rate <= 0 || options.channel_count <= 0) {
      return WaveformResult<Waveform>::failure(
          {.code = WaveformErrorCode::InvalidArgument, .message = "invalid waveform options"});
    }
    if (asset_id.empty()) {
      return WaveformResult<Waveform>::failure(
          {.code = WaveformErrorCode::InvalidArgument, .message = "asset id is empty"});
    }

    const CacheKey key{.asset_id = asset_id,
                      .kind = CacheKind::Waveform,
                      .parameter_hash = waveform_parameter_hash(options)};
    if (!key.valid()) {
      return WaveformResult<Waveform>::failure(
          {.code = WaveformErrorCode::InvalidArgument, .message = "invalid cache key"});
    }

    // Serve from cache when a valid entry already exists.
    CacheResult<bool> present = cache.contains(key);
    if (!present.has_value()) {
      return WaveformResult<Waveform>::failure(
          {.code = WaveformErrorCode::StoreFailed,
           .native_code = present.error().native_code,
           .message = present.error().message});
    }
    if (present.value()) {
      return load_waveform(asset_id, options, cache);
    }

    check_cancelled(cancellation);
    if (!std::filesystem::exists(asset_uri)) {
      fail(WaveformErrorCode::SourceNotFound, "waveform source does not exist");
    }

    // Open input and inspect streams.
    InterruptState interrupt{.cancellation = cancellation};
    AVFormatContext* raw_input = avformat_alloc_context();
    if (raw_input == nullptr) {
      fail(WaveformErrorCode::Internal, "cannot allocate an input format context", AVERROR(ENOMEM));
    }
    raw_input->interrupt_callback = {.callback = interrupt_callback, .opaque = &interrupt};
    media::apply_input_probe_options(*raw_input);
    const std::string path = native_path(asset_uri);
    const int open_result = avformat_open_input(&raw_input, path.c_str(), nullptr, nullptr);
    InputFormat input(raw_input); // takes ownership regardless of open result
    if (open_result < 0) {
      check_cancelled(cancellation);
      require_ffmpeg(open_result, WaveformErrorCode::OpenFailed, "open waveform source");
    }
    const int stream_info_result = media::inspect_input_streams(*input);
    if (stream_info_result < 0) {
      check_cancelled(cancellation);
      require_ffmpeg(stream_info_result, WaveformErrorCode::OpenFailed,
                     "inspect waveform source streams");
    }

    // Select the audio stream.
    int audio_index = stream_index;
    if (audio_index < 0) {
      audio_index =
          av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
    if (audio_index < 0) {
      fail(WaveformErrorCode::NoAudioStream, "source has no audio stream", audio_index);
    }
    AVStream* stream = input->streams[static_cast<unsigned>(audio_index)];
    if (stream == nullptr || stream->codecpar == nullptr ||
        stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
      fail(WaveformErrorCode::NoAudioStream, "selected stream is not audio");
    }

    // Open the decoder.
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
      fail(WaveformErrorCode::DecodeFailed, "no decoder is available for the audio stream");
    }
    CodecContext codec(avcodec_alloc_context3(decoder));
    if (!codec) {
      fail(WaveformErrorCode::Internal, "cannot allocate an audio decoder context", AVERROR(ENOMEM));
    }
    require_ffmpeg(avcodec_parameters_to_context(codec.get(), stream->codecpar),
                   WaveformErrorCode::OpenFailed, "copy audio decoder parameters");
    codec->pkt_timebase = stream->time_base;
    require_ffmpeg(avcodec_open2(codec.get(), decoder, nullptr), WaveformErrorCode::DecodeFailed,
                   "open audio decoder");

    // Configure the resampler: source layout/format/rate -> mono (or
    // requested channel count) float32 at the requested sample rate.
    AVChannelLayout in_layout{};
    const AVChannelLayout* src_layout = &codec->ch_layout;
    if (src_layout->nb_channels <= 0 || src_layout->order == AV_CHANNEL_ORDER_UNSPEC) {
      const int fallback_channels = src_layout->nb_channels <= 0 ? 1 : src_layout->nb_channels;
      av_channel_layout_default(&in_layout, fallback_channels);
      src_layout = &in_layout;
    }
    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, static_cast<int>(options.channel_count));

    SwrContext* raw_swr = nullptr;
    require_ffmpeg(swr_alloc_set_opts2(&raw_swr, &out_layout, AV_SAMPLE_FMT_FLT,
                                       static_cast<int>(options.sample_rate), src_layout,
                                       codec->sample_fmt, codec->sample_rate, 0, nullptr),
                   WaveformErrorCode::ResampleFailed, "configure waveform resampler");
    ResampleContext resampler(raw_swr);
    require_ffmpeg(swr_init(resampler.get()), WaveformErrorCode::ResampleFailed,
                   "initialize waveform resampler");

    // Decode all packets, resample, and accumulate float samples.
    std::vector<float> samples;
    Packet packet = make_packet();
    Frame decoded = make_frame();
    for (;;) {
      check_cancelled(cancellation);
      const int read_result = av_read_frame(input.get(), packet.get());
      if (read_result == AVERROR_EOF) {
        break;
      }
      require_ffmpeg(read_result, WaveformErrorCode::DecodeFailed, "read waveform packet");
      if (packet->stream_index != audio_index) {
        av_packet_unref(packet.get());
        continue;
      }
      require_ffmpeg(avcodec_send_packet(codec.get(), packet.get()),
                     WaveformErrorCode::DecodeFailed, "send waveform packet");
      for (;;) {
        const int recv = avcodec_receive_frame(codec.get(), decoded.get());
        if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) {
          break;
        }
        require_ffmpeg(recv, WaveformErrorCode::DecodeFailed, "decode waveform frame");
        append_resampled(decoded.get(), codec, resampler, out_layout, options.sample_rate,
                         options.channel_count, samples);
        av_frame_unref(decoded.get());
      }
      av_packet_unref(packet.get());
    }

    // Flush the decoder.
    require_ffmpeg(avcodec_send_packet(codec.get(), nullptr), WaveformErrorCode::DecodeFailed,
                   "flush waveform decoder");
    for (;;) {
      const int recv = avcodec_receive_frame(codec.get(), decoded.get());
      if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) {
        break;
      }
      require_ffmpeg(recv, WaveformErrorCode::DecodeFailed, "decode flushed waveform frame");
      append_resampled(decoded.get(), codec, resampler, out_layout, options.sample_rate,
                       options.channel_count, samples);
      av_frame_unref(decoded.get());
    }

    // Flush the resampler.
    flush_resampler(codec, resampler, out_layout, options.sample_rate, options.channel_count,
                    samples);

    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    // The pyramid is always mono; mix down any multi-channel decode output.
    std::vector<float> mono = mix_to_mono(samples, options.channel_count);

    Waveform waveform = build_waveform_pyramid(mono, options.sample_rate, options);
    waveform.source_stream_index = audio_index;

    const std::vector<std::byte> blob = serialize_waveform(waveform);
    CacheResult<void> stored = cache.put(key, blob);
    if (!stored.has_value()) {
      return WaveformResult<Waveform>::failure(
          {.code = WaveformErrorCode::StoreFailed,
           .native_code = stored.error().native_code,
           .message = stored.error().message});
    }
    return WaveformResult<Waveform>::success(std::move(waveform));
  } catch (const WaveformFailure& failure) {
    return WaveformResult<Waveform>::failure(failure.error());
  }
}

} // namespace video_editor::media_cache
