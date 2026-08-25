// SPDX-License-Identifier: MPL-2.0
#include "video_editor/proxy_service/proxy_service.h"

#include "video_editor/media_codec/probe.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace video_editor::proxy {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("video_editor_proxy_service_test_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string shell_quote(const std::string_view value) {
#ifdef _WIN32
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '"') {
      quoted += '\\';
    }
    quoted += character;
  }
  quoted += '"';
#else
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += '\'';
#endif
  return quoted;
}

[[nodiscard]] bool create_fixture(const std::filesystem::path& path) {
#ifndef VIDEO_EDITOR_TEST_FFMPEG
  static_cast<void>(path);
  return false;
#else
  const std::vector<std::string> arguments{
      VIDEO_EDITOR_TEST_FFMPEG,
      "-hide_banner",
      "-loglevel",
      "error",
      "-nostdin",
      "-y",
      "-f",
      "lavfi",
      "-i",
      "testsrc2=size=322x182:rate=25:duration=1.2",
      "-f",
      "lavfi",
      "-i",
      "sine=frequency=1000:sample_rate=44100:duration=1.2",
      "-filter_complex",
      "[0:v]setpts=5/TB+(N+floor(N/3))/(25*TB),setsar=4/3,"
      "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709[v];"
      "[1:a]asetpts=PTS+5.12/TB[a]",
      "-map",
      "[v]",
      "-map",
      "[a]",
      "-fps_mode",
      "vfr",
      "-c:v",
      "mpeg4",
      "-bf",
      "2",
      "-g",
      "12",
      "-q:v",
      "3",
      "-c:a",
      "pcm_s16le",
      "-metadata:s:v:0",
      "rotate=90",
      "-color_primaries",
      "bt709",
      "-color_trc",
      "bt709",
      "-colorspace",
      "bt709",
      path.string(),
  };
  std::ostringstream command;
  for (const std::string& argument : arguments) {
    if (command.tellp() > 0) {
      command << ' ';
    }
    command << shell_quote(argument);
  }
  return std::system(command.str().c_str()) == 0;
#endif
}

struct FormatDeleter {
  void operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
      AVFormatContext* local = context;
      avformat_close_input(&local);
    }
  }
};

struct CodecDeleter {
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

struct DecodedVideo {
  AVRational time_base{0, 1};
  AVRational sample_aspect_ratio{0, 1};
  std::vector<std::int64_t> pts;
};

[[nodiscard]] DecodedVideo decode_video(const std::filesystem::path& path) {
  AVFormatContext* raw_format = nullptr;
  const int open_result = avformat_open_input(&raw_format, path.string().c_str(), nullptr, nullptr);
  EXPECT_GE(open_result, 0);
  std::unique_ptr<AVFormatContext, FormatDeleter> format(raw_format);
  if (open_result < 0 || !format) {
    return {};
  }
  const int info_result = avformat_find_stream_info(format.get(), nullptr);
  EXPECT_GE(info_result, 0);
  if (info_result < 0) {
    return {};
  }
  const int stream_index =
      av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  EXPECT_GE(stream_index, 0);
  if (stream_index < 0) {
    return {};
  }
  AVStream* stream = format->streams[static_cast<unsigned>(stream_index)];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  EXPECT_NE(codec, nullptr);
  if (codec == nullptr) {
    return {};
  }
  std::unique_ptr<AVCodecContext, CodecDeleter> decoder(avcodec_alloc_context3(codec));
  EXPECT_NE(decoder, nullptr);
  if (!decoder) {
    return {};
  }
  const int parameters_result = avcodec_parameters_to_context(decoder.get(), stream->codecpar);
  EXPECT_GE(parameters_result, 0);
  if (parameters_result < 0) {
    return {};
  }
  decoder->pkt_timebase = stream->time_base;
  const int decoder_result = avcodec_open2(decoder.get(), codec, nullptr);
  EXPECT_GE(decoder_result, 0);
  if (decoder_result < 0) {
    return {};
  }
  std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
  std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
  EXPECT_NE(packet, nullptr);
  EXPECT_NE(frame, nullptr);
  if (!packet || !frame) {
    return {};
  }

  DecodedVideo decoded{
      .time_base = stream->time_base,
      .sample_aspect_ratio = stream->sample_aspect_ratio.num > 0
                                 ? stream->sample_aspect_ratio
                                 : stream->codecpar->sample_aspect_ratio,
      .pts = {},
  };
  const auto drain = [&]() {
    for (;;) {
      const int result = avcodec_receive_frame(decoder.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return;
      }
      ASSERT_GE(result, 0);
      decoded.pts.push_back(frame->best_effort_timestamp != AV_NOPTS_VALUE
                                ? frame->best_effort_timestamp
                                : frame->pts);
      av_frame_unref(frame.get());
    }
  };
  while (av_read_frame(format.get(), packet.get()) >= 0) {
    if (packet->stream_index == stream_index) {
      const int send_result = avcodec_send_packet(decoder.get(), packet.get());
      EXPECT_GE(send_result, 0);
      if (send_result < 0) {
        return decoded;
      }
      drain();
    }
    av_packet_unref(packet.get());
  }
  const int drain_result = avcodec_send_packet(decoder.get(), nullptr);
  EXPECT_GE(drain_result, 0);
  if (drain_result < 0) {
    return decoded;
  }
  drain();
  return decoded;
}

void write_text(const std::filesystem::path& path, const std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(ProxyProfile, ResolvesProResAndDeterministicPatentNeutralFallback) {
  const EncoderAvailability all{
      .prores_proxy = true,
      .ffv1 = true,
      .pcm_s16le = true,
      .prores_encoder = "prores_test",
      .ffv1_encoder = "ffv1_test",
      .pcm_encoder = "pcm_test",
  };
  const auto default_result = resolve_profile({}, all);
  ASSERT_TRUE(default_result);
  EXPECT_EQ(default_result.value().video_codec, VideoCodec::ProResProxy);
  EXPECT_EQ(default_result.value().container, Container::QuickTime);
  EXPECT_FALSE(default_result.value().used_fallback);

  EncoderAvailability no_prores = all;
  no_prores.prores_proxy = false;
  no_prores.prores_encoder.clear();
  const auto first = resolve_profile({}, no_prores);
  const auto second = resolve_profile({}, no_prores);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value(), second.value());
  EXPECT_EQ(first.value().video_codec, VideoCodec::Ffv1);
  EXPECT_EQ(first.value().container, Container::Matroska);
  EXPECT_TRUE(first.value().used_fallback);
}

TEST(ProxyProfile, PreflightsUnavailableAudioAndEncoders) {
  EncoderAvailability unavailable;
  const auto result = resolve_profile({}, unavailable);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::EncoderUnavailable);

  ProxyProfile silent;
  silent.include_pcm_audio = false;
  silent.allow_ffv1_fallback = false;
  const auto silent_result = resolve_profile(silent, unavailable);
  ASSERT_FALSE(silent_result);
  EXPECT_EQ(silent_result.error().code, ErrorCode::EncoderUnavailable);

  ProxyProfile invalid;
  invalid.video_codec = static_cast<VideoCodec>(255);
  const auto invalid_result = resolve_profile(invalid, unavailable);
  ASSERT_FALSE(invalid_result);
  EXPECT_EQ(invalid_result.error().code, ErrorCode::InvalidArgument);
}

TEST(ProxyPtsMap, RejectsCorruptAndFutureData) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "bad.vepts";
  write_text(path, "not-a-map");
  const auto result = load_pts_map(path);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::InvalidPtsMap);

  const auto future_path = directory.path() / "future.vepts";
  std::ofstream future(future_path, std::ios::binary | std::ios::trunc);
  const std::array<unsigned char, 12> bytes{'V', 'E', 'P', 'T', 'S', 'M', 'A', 'P', 2, 0, 0, 0};
  future.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  future.close();
  const auto future_result = load_pts_map(future_path);
  ASSERT_FALSE(future_result);
  EXPECT_EQ(future_result.error().code, ErrorCode::InvalidPtsMap);
}

TEST(ProxyPtsLookup, FindsFramesBySourceAndProxyPts) {
  StreamPtsMap stream{
      .source_stream_index = 0,
      .proxy_stream_index = 0,
      .source_time_base = {1, 25},
      .proxy_time_base = {1, 25},
      .source_origin_pts = 100,
      .frames = {{.source_pts = 100,
                  .source_duration = 25,
                  .proxy_pts = 0,
                  .proxy_duration = 25},
                 {.source_pts = 125,
                  .source_duration = 25,
                  .proxy_pts = 25,
                  .proxy_duration = 25},
                 {.source_pts = 150,
                  .source_duration = 25,
                  .proxy_pts = 50,
                  .proxy_duration = 25}},
  };
  const PtsMap map{.source_fingerprint = {}, .streams = {stream}};

  ASSERT_NE(stream_pts_map(map, 0), nullptr);
  ASSERT_NE(stream_pts_map(map, -1), nullptr);
  EXPECT_EQ(*stream_pts_map(map, -1), stream);

  const auto first = lookup_frame_by_source_pts(stream, 110);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->source_pts, 100);
  EXPECT_EQ(first->proxy_pts, 0);

  const auto second = lookup_frame_by_source_pts(stream, 149);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->source_pts, 125);

  EXPECT_FALSE(lookup_frame_by_source_pts(stream, 99).has_value());
  EXPECT_FALSE(lookup_frame_by_source_pts(stream, 175).has_value());

  const auto by_proxy = lookup_frame_by_proxy_pts(stream, 50);
  ASSERT_TRUE(by_proxy.has_value());
  EXPECT_EQ(by_proxy->source_pts, 150);
}

TEST(ProxyGeneration, ReencodesVfrBFramesWithExactPtsMapAndAudio) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "unusual-start-vfr.mkv";
  if (!create_fixture(source)) {
    GTEST_SKIP() << "the pinned ffmpeg fixture generator is unavailable";
  }
  const auto destination = directory.path() / "proxy.mkv";
  GenerateRequest request{
      .source = source,
      .destination = destination,
      .pts_map_destination = std::nullopt,
      .profile = patent_neutral_fallback_profile(),
  };
  std::vector<ProgressStage> stages;
  const auto generated = generate_proxy(
      request, {}, [&stages](const Progress& progress) { stages.push_back(progress.stage); });
  ASSERT_TRUE(generated) << generated.error().message;
  EXPECT_EQ(generated.value().profile.video_codec, VideoCodec::Ffv1);
  EXPECT_EQ(generated.value().profile.container, Container::Matroska);
  EXPECT_EQ(generated.value().width, 160);
  EXPECT_EQ(generated.value().height, 90);
  EXPECT_TRUE(generated.value().audio_included);
  EXPECT_EQ(generated.value().scanned_source_streams, 2U);
  EXPECT_GT(generated.value().audio_samples, 0U);
  ASSERT_FALSE(stages.empty());
  EXPECT_EQ(stages.front(), ProgressStage::Inspecting);
  EXPECT_EQ(stages.back(), ProgressStage::Complete);

  const auto descriptor = media::probe(destination);
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  ASSERT_GE(descriptor.value().best_video_stream, 0);
  ASSERT_GE(descriptor.value().best_audio_stream, 0);
  const auto& video =
      descriptor.value().streams.at(static_cast<std::size_t>(descriptor.value().best_video_stream));
  const auto& audio =
      descriptor.value().streams.at(static_cast<std::size_t>(descriptor.value().best_audio_stream));
  ASSERT_TRUE(video.video.has_value());
  ASSERT_TRUE(audio.audio.has_value());
  EXPECT_EQ(video.codec_name, "ffv1");
  EXPECT_EQ(video.video->width, 160);
  EXPECT_EQ(video.video->height, 90);
  EXPECT_EQ(video.video->rotation_degrees, 90);
  EXPECT_EQ(video.video->color.primaries, "bt709");
  EXPECT_EQ(audio.audio->sample_rate, 48'000);
  EXPECT_EQ(audio.codec_name, "pcm_s16le");

  const auto source_descriptor = media::probe(source);
  ASSERT_TRUE(source_descriptor) << source_descriptor.error().message;
  const auto stream_start_microseconds = [](const media::StreamDescriptor& stream) {
    EXPECT_TRUE(stream.start_time.has_value());
    if (!stream.start_time.has_value()) {
      return std::int64_t{0};
    }
    return av_rescale_q(*stream.start_time,
                        AVRational{static_cast<int>(stream.time_base.numerator),
                                   static_cast<int>(stream.time_base.denominator)},
                        AVRational{1, AV_TIME_BASE});
  };
  const auto& source_video = source_descriptor.value().streams.at(
      static_cast<std::size_t>(source_descriptor.value().best_video_stream));
  const auto& source_audio = source_descriptor.value().streams.at(
      static_cast<std::size_t>(source_descriptor.value().best_audio_stream));
  const std::int64_t source_offset =
      stream_start_microseconds(source_audio) - stream_start_microseconds(source_video);
  const std::int64_t proxy_offset =
      stream_start_microseconds(audio) - stream_start_microseconds(video);
  EXPECT_NEAR(static_cast<double>(proxy_offset), static_cast<double>(source_offset),
              1'000'000.0 / 48'000.0);

  const auto loaded = load_pts_map(generated.value().pts_map_path);
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded.value(), generated.value().pts_map);
  ASSERT_EQ(loaded.value().streams.size(), 1U);
  const StreamPtsMap& map = loaded.value().streams.front();

  const DecodedVideo original = decode_video(source);
  const DecodedVideo proxy = decode_video(destination);
  EXPECT_EQ(proxy.sample_aspect_ratio.num, 4);
  EXPECT_EQ(proxy.sample_aspect_ratio.den, 3);
  ASSERT_EQ(map.frames.size(), original.pts.size());
  ASSERT_EQ(map.frames.size(), proxy.pts.size());
  ASSERT_EQ(map.frames.size(), generated.value().video_frames);
  EXPECT_GT(map.frames.size(), 10U);
  for (std::size_t index = 0; index < map.frames.size(); ++index) {
    EXPECT_EQ(map.frames[index].source_pts, original.pts[index]);
    EXPECT_EQ(map.frames[index].proxy_pts, proxy.pts[index]);
    const AVRational source_time_base{map.source_time_base.numerator,
                                      map.source_time_base.denominator};
    const AVRational proxy_time_base{map.proxy_time_base.numerator,
                                     map.proxy_time_base.denominator};
    EXPECT_EQ(map.frames[index].proxy_pts,
              av_rescale_q(map.frames[index].source_pts - map.source_origin_pts, source_time_base,
                           proxy_time_base));
    if (index > 0) {
      EXPECT_GE(map.frames[index].source_pts, map.frames[index - 1].source_pts);
      EXPECT_GE(map.frames[index].proxy_pts, map.frames[index - 1].proxy_pts);
    }
  }

  bool observed_vfr_gap = false;
  for (std::size_t index = 2; index < original.pts.size(); ++index) {
    observed_vfr_gap = observed_vfr_gap || original.pts[index] - original.pts[index - 1] !=
                                               original.pts[index - 1] - original.pts[index - 2];
  }
  EXPECT_TRUE(observed_vfr_gap);
}

TEST(ProxyGeneration, DefaultProfileProducesHalfResolutionProResMov) {
  const EncoderAvailability availability = encoder_availability();
  if (!availability.prores_proxy || !availability.pcm_s16le) {
    GTEST_SKIP() << "the default ProRes Proxy/PCM encoders are unavailable";
  }
  TemporaryDirectory directory;
  const auto source = directory.path() / "default-source.mkv";
  if (!create_fixture(source)) {
    GTEST_SKIP() << "the pinned ffmpeg fixture generator is unavailable";
  }
  const auto destination = directory.path() / "default.mov";
  write_text(destination, "replace-this-complete-file");
  write_text(default_pts_map_path(destination), "replace-this-complete-map");
  const auto generated = generate_proxy({.source = source,
                                         .destination = destination,
                                         .pts_map_destination = std::nullopt,
                                         .profile = {}});
  ASSERT_TRUE(generated) << generated.error().message;
  EXPECT_EQ(generated.value().profile.video_codec, VideoCodec::ProResProxy);
  EXPECT_EQ(generated.value().profile.container, Container::QuickTime);
  EXPECT_FALSE(generated.value().profile.used_fallback);
  EXPECT_EQ(generated.value().width, 160);
  EXPECT_EQ(generated.value().height, 90);
  EXPECT_TRUE(generated.value().audio_included);
  const auto descriptor = media::probe(destination);
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  EXPECT_EQ(descriptor.value().format_name, "mov,mp4,m4a,3gp,3g2,mj2");
  const auto& video =
      descriptor.value().streams.at(static_cast<std::size_t>(descriptor.value().best_video_stream));
  const auto& audio =
      descriptor.value().streams.at(static_cast<std::size_t>(descriptor.value().best_audio_stream));
  EXPECT_EQ(video.codec_name, "prores");
  EXPECT_EQ(audio.codec_name, "pcm_s16le");
  EXPECT_NE(read_text(destination), "replace-this-complete-file");
  const auto map = load_pts_map(default_pts_map_path(destination));
  ASSERT_TRUE(map) << map.error().message;
  EXPECT_EQ(map.value().video_codec, VideoCodec::ProResProxy);
  EXPECT_EQ(map.value().container, Container::QuickTime);
}

TEST(ProxyGeneration, CancellationLeavesExistingDestinationsUntouched) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "cancel-source.mkv";
  if (!create_fixture(source)) {
    GTEST_SKIP() << "the pinned ffmpeg fixture generator is unavailable";
  }
  const auto destination = directory.path() / "existing-proxy.mkv";
  const auto map_path = default_pts_map_path(destination);
  write_text(destination, "authoritative-old-proxy");
  write_text(map_path, "authoritative-old-map");

  std::stop_source cancellation;
  const GenerateRequest request{
      .source = source,
      .destination = destination,
      .pts_map_destination = std::nullopt,
      .profile = patent_neutral_fallback_profile(),
  };
  const auto result =
      generate_proxy(request, cancellation.get_token(), [&cancellation](const Progress& progress) {
        if (progress.stage == ProgressStage::Transcoding && progress.video_frames >= 2U) {
          cancellation.request_stop();
        }
      });
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
  EXPECT_EQ(read_text(destination), "authoritative-old-proxy");
  EXPECT_EQ(read_text(map_path), "authoritative-old-map");

  for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
    EXPECT_EQ(entry.path().filename().string().find(".partial."), std::string::npos);
  }
}

TEST(ProxyGeneration, AlreadyCancelledRequestDoesNotCreateDestinations) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.bin";
  write_text(source, "source");
  const auto destination = directory.path() / "never-created.mkv";
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto result = generate_proxy({.source = source,
                                      .destination = destination,
                                      .pts_map_destination = std::nullopt,
                                      .profile = patent_neutral_fallback_profile()},
                                     cancellation.get_token());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
  EXPECT_FALSE(std::filesystem::exists(destination));
  EXPECT_FALSE(std::filesystem::exists(default_pts_map_path(destination)));
}

template <typename Integer>
void append_little_endian(std::vector<std::byte>& output, const Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto bits = static_cast<Unsigned>(value);
  for (std::size_t shift = 0; shift < sizeof(Integer); ++shift) {
    output.push_back(
        static_cast<std::byte>((bits >> (shift * 8U)) & static_cast<Unsigned>(0xFFU)));
  }
}

void append_prefixed_string(std::vector<std::byte>& output, const std::string& value) {
  append_little_endian(output, static_cast<std::uint32_t>(value.size()));
  for (const char byte : value) {
    output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
  }
}

[[nodiscard]] std::vector<std::byte> make_pts_map_bytes(const assets::FileFingerprint& fingerprint,
                                                        const VideoCodec codec,
                                                        const Container container) {
  std::vector<std::byte> output;
  for (const char byte : std::string_view{"VEPTSMAP"}) {
    output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
  }
  append_little_endian(output, std::uint32_t{1});
  append_little_endian(output, static_cast<std::uint8_t>(codec));
  append_little_endian(output, static_cast<std::uint8_t>(container));
  append_little_endian(output, std::uint16_t{0});
  append_little_endian(output, static_cast<std::uint64_t>(fingerprint.size));
  append_little_endian(output, fingerprint.modified_nanoseconds);
  append_prefixed_string(output, fingerprint.quick_sha256);
  append_little_endian(output, std::uint8_t{0});
  append_little_endian(output, std::uint32_t{1});
  append_little_endian(output, std::int32_t{0});
  append_little_endian(output, std::int32_t{0});
  append_little_endian(output, std::int32_t{1});
  append_little_endian(output, std::int32_t{25});
  append_little_endian(output, std::int32_t{1});
  append_little_endian(output, std::int32_t{25});
  append_little_endian(output, std::int64_t{0});
  append_little_endian(output, std::uint64_t{0});
  return output;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] assets::FileFingerprint make_fingerprint(const std::uint64_t size, const char fill) {
  assets::FileFingerprint fingerprint;
  fingerprint.size = size;
  fingerprint.modified_nanoseconds = 1;
  fingerprint.quick_sha256 = std::string(64, fill);
  return fingerprint;
}

TEST(ProxyDiscovery, ParameterHashIsStableForDefaultAndResolvedProfiles) {
  EXPECT_EQ(proxy_parameter_hash(ProxyProfile{}), proxy_parameter_hash(ProxyProfile{}));
  const ProxyProfile fallback = patent_neutral_fallback_profile();
  EXPECT_NE(proxy_parameter_hash(ProxyProfile{}), proxy_parameter_hash(fallback));
  ResolvedProfile resolved;
  resolved.requested = ProxyProfile{};
  resolved.video_codec = VideoCodec::ProResProxy;
  resolved.container = Container::QuickTime;
  resolved.used_fallback = false;
  EXPECT_NE(proxy_parameter_hash(ProxyProfile{}), proxy_parameter_hash(resolved));
  EXPECT_EQ(proxy_parameter_hash(resolved), proxy_parameter_hash(resolved));
}

TEST(ProxyDiscovery, FindsCompleteProxyInCacheStore) {
  TemporaryDirectory directory;
  media_cache::CacheStore cache(directory.path() / "cache");
  const std::string asset_id = "asset-discover";
  const auto fingerprint = make_fingerprint(2048, 'a');
  const std::string hash = proxy_parameter_hash(ProxyProfile{});

  const auto proxy_source = directory.path() / "proxy.mov";
  write_text(proxy_source, "dummy-proxy-bytes");
  ASSERT_TRUE(cache
                  .put_file({.asset_id = asset_id,
                             .kind = media_cache::CacheKind::Proxy,
                             .parameter_hash = hash},
                            proxy_source)
                  .has_value());

  const auto pts_source = directory.path() / "proxy.vepts";
  write_bytes(pts_source, make_pts_map_bytes(fingerprint, VideoCodec::ProResProxy,
                                             Container::QuickTime));
  ASSERT_TRUE(cache
                  .put_file({.asset_id = asset_id,
                             .kind = media_cache::CacheKind::ProxyPtsMap,
                             .parameter_hash = hash},
                            pts_source)
                  .has_value());

  const auto found = discover_proxy(asset_id, fingerprint, cache);
  ASSERT_TRUE(found.has_value());
  EXPECT_TRUE(found->manifest.complete);
  EXPECT_EQ(found->manifest.engine_version, "proxy-service-v1");
  EXPECT_TRUE(found->manifest.source_fingerprint.content_matches(fingerprint));
  EXPECT_EQ(found->manifest.profile.codec, assets::ProxyCodec::ProResProxy);
  EXPECT_EQ(found->manifest.profile.maximum_width, 1920);
  EXPECT_EQ(found->manifest.profile.maximum_height, 1080);
  EXPECT_TRUE(found->manifest.profile.include_pcm_audio);
  EXPECT_TRUE(std::filesystem::exists(found->manifest.proxy_uri));
  EXPECT_TRUE(std::filesystem::exists(found->pts_map_path));

  const auto mismatch = discover_proxy(asset_id, make_fingerprint(2048, 'b'), cache);
  EXPECT_FALSE(mismatch.has_value());
}

TEST(ProxyDiscovery, FindsLegacyDirectoryProxyAndIgnoresIncompleteFiles) {
  TemporaryDirectory directory;
  media_cache::CacheStore cache(directory.path() / "cache");
  const std::string asset_id = "legacy-asset";
  const auto fingerprint = make_fingerprint(512, 'c');
  const auto legacy = directory.path() / "legacy";
  std::filesystem::create_directories(legacy);

  const auto incomplete = legacy / (asset_id + ".proxy.mkv");
  write_text(incomplete, "incomplete-proxy");

  EXPECT_FALSE(discover_proxy(asset_id, fingerprint, cache, legacy).has_value());

  const auto proxy = legacy / (asset_id + ".proxy.mov");
  write_text(proxy, "legacy-proxy");
  write_bytes(default_pts_map_path(proxy),
              make_pts_map_bytes(fingerprint, VideoCodec::Ffv1, Container::Matroska));

  const auto found = discover_proxy(asset_id, fingerprint, cache, legacy);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->manifest.proxy_uri, proxy);
  EXPECT_EQ(found->pts_map_path, default_pts_map_path(proxy));
  EXPECT_EQ(found->manifest.profile.codec, assets::ProxyCodec::Ffv1);
  EXPECT_TRUE(found->manifest.complete);
}

} // namespace
} // namespace video_editor::proxy
