// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/original_audio_registry.h"
#include "video_editor/export_service/export_service.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mathematics.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace video_editor::export_service {
namespace {

struct InputFormatCloser final {
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

using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatCloser>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;

[[nodiscard]] std::string ffmpeg_error(const int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  av_strerror(error, buffer.data(), buffer.size());
  return buffer.data();
}

[[nodiscard]] std::string path_string(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

void write_u16(std::ofstream& output, const std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value & 0xffU),
                                  static_cast<char>((value >> 8U) & 0xffU)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ofstream& output, const std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value & 0xffU), static_cast<char>((value >> 8U) & 0xffU),
      static_cast<char>((value >> 16U) & 0xffU), static_cast<char>((value >> 24U) & 0xffU)};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_constant_pcm_wav(const std::filesystem::path& path, const std::size_t sample_count) {
  constexpr std::uint16_t channels = 2;
  constexpr std::uint16_t bits_per_sample = 16;
  constexpr std::int16_t amplitude = 4'096;
  const auto data_size = static_cast<std::uint32_t>(sample_count * channels * sizeof(std::int16_t));
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("could not create export audio fixture");
  }
  output.write("RIFF", 4);
  write_u32(output, 36U + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16U);
  write_u16(output, 1U);
  write_u16(output, channels);
  write_u32(output, audio_render::kTimelineAudioSampleRate);
  write_u32(output, audio_render::kTimelineAudioSampleRate * channels * (bits_per_sample / 8U));
  write_u16(output, channels * (bits_per_sample / 8U));
  write_u16(output, bits_per_sample);
  output.write("data", 4);
  write_u32(output, data_size);
  for (std::size_t index = 0; index < sample_count; ++index) {
    write_u16(output, static_cast<std::uint16_t>(amplitude));
    write_u16(output, static_cast<std::uint16_t>(amplitude));
  }
  if (!output) {
    throw std::runtime_error("could not finish export audio fixture");
  }
}

class TestDirectory final {
public:
  TestDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("video-editor-export-test-" + edit::EntityId::generate().toString())) {
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class DeterministicProvider final : public render::FrameProvider {
public:
  std::vector<render::AssetFrameRequest> requests;

  render::RenderResult<std::shared_ptr<const render::CpuFrame>>
  request(const render::AssetFrameRequest& request) override {
    requests.push_back(request);
    auto frame =
        std::make_shared<render::CpuFrame>(request.preferred_width, request.preferred_height);
    const auto tick =
        request.source_time.rescaledTo(60, edit::RoundingMode::NearestTiesEven).value();
    const float red = static_cast<float>((tick % 7) + 1) / 8.0F;
    for (int y = 0; y < frame->height(); ++y) {
      for (int x = 0; x < frame->width(); ++x) {
        auto pixel = frame->pixel(x, y);
        pixel[0] = red;
        pixel[1] = static_cast<float>(x + 1) / static_cast<float>(frame->width() + 1);
        pixel[2] = static_cast<float>(y + 1) / static_cast<float>(frame->height() + 1);
        pixel[3] = 1.0F;
      }
    }
    return render::RenderResult<std::shared_ptr<const render::CpuFrame>>::success(
        std::static_pointer_cast<const render::CpuFrame>(frame));
  }
};

class CancellingOriginalProvider final : public audio_render::OriginalAudioProvider {
public:
  CancellingOriginalProvider(std::filesystem::path path, std::stop_source& cancellation)
      : path_(std::move(path)), cancellation_(&cancellation) {}

  [[nodiscard]] std::optional<audio_render::OriginalAudioMedia>
  resolve_original(const edit::EntityId&) const override {
    cancellation_->request_stop();
    return audio_render::OriginalAudioMedia{.path = path_, .audio_stream_index = -1};
  }

private:
  std::filesystem::path path_;
  std::stop_source* cancellation_;
};

struct TestTimeline final {
  edit::TimelineSnapshot snapshot;
  std::shared_ptr<DeterministicProvider> provider;
  std::shared_ptr<render::CpuRenderer> renderer;
  std::shared_ptr<audio_render::TimelineAudioRenderer> audio_renderer;
  edit::EntityId asset_id;
};

[[nodiscard]] TestTimeline
make_timeline(const std::optional<std::filesystem::path>& original_audio = std::nullopt,
              std::shared_ptr<audio_render::OriginalAudioProvider> audio_provider = nullptr) {
  edit::Project project;
  edit::Asset asset;
  asset.name = "Generated source";
  asset.source_uri = "memory://generated";
  asset.duration = edit::Time(7, 60);
  asset.has_video = true;
  asset.width = 4;
  asset.height = 2;
  asset.nominal_frame_rate = edit::Rate(30, 1);
  if (original_audio.has_value() || audio_provider != nullptr) {
    asset.source_uri = "proxy://must-not-be-used-for-export";
    asset.has_audio = true;
    asset.audio_sample_rate = audio_render::kTimelineAudioSampleRate;
    asset.audio_channels = audio_render::kTimelineAudioChannels;
  }
  const edit::EntityId asset_id = asset.id;

  edit::Clip clip;
  clip.asset_id = asset.id;
  clip.timeline_range = {edit::Time{}, asset.duration};
  clip.source_range = clip.timeline_range;

  edit::Track track;
  track.kind = edit::TrackKind::Video;
  track.clips.push_back(clip);

  edit::Sequence sequence;
  sequence.name = "Four-frame test";
  sequence.width = 4;
  sequence.height = 2;
  sequence.frame_rate = edit::Rate(30, 1);
  sequence.tracks.push_back(track);
  if (original_audio.has_value() || audio_provider != nullptr) {
    edit::Clip audio_clip = clip;
    audio_clip.id = edit::EntityId::generate();
    audio_clip.kind = edit::ClipKind::Audio;
    // Hard-left panning makes the expected PCM fixture exact: left is 4096,
    // right is zero after the renderer's constant-power pan stage.
    audio_clip.audio_pan = -1.0;
    edit::Track audio_track;
    audio_track.kind = edit::TrackKind::Audio;
    audio_track.clips.push_back(audio_clip);
    sequence.tracks.push_back(std::move(audio_track));
  }
  const edit::EntityId sequence_id = sequence.id;

  project.assets.push_back(asset);
  project.sequences.push_back(sequence);
  edit::TimelineEditor editor(std::move(project));
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  if (!snapshot) {
    throw std::runtime_error(snapshot.error().message);
  }
  auto provider = std::make_shared<DeterministicProvider>();
  auto renderer = std::make_shared<render::CpuRenderer>(provider);
  if (original_audio.has_value()) {
    auto registry = std::make_shared<audio_render::OriginalAudioRegistry>();
    if (!registry->register_original(asset_id, {*original_audio, -1})) {
      throw std::runtime_error("could not register export audio fixture");
    }
    audio_provider = std::move(registry);
  }
  std::shared_ptr<audio_render::TimelineAudioRenderer> audio_renderer;
  if (audio_provider != nullptr) {
    audio_renderer =
        std::make_shared<audio_render::TimelineAudioRenderer>(std::move(audio_provider));
  }
  return {.snapshot = std::move(snapshot).value(),
          .provider = std::move(provider),
          .renderer = std::move(renderer),
          .audio_renderer = std::move(audio_renderer),
          .asset_id = asset_id};
}

struct DecodedVideo final {
  AVCodecID codec_id{AV_CODEC_ID_NONE};
  AVColorPrimaries primaries{AVCOL_PRI_UNSPECIFIED};
  AVColorTransferCharacteristic transfer{AVCOL_TRC_UNSPECIFIED};
  AVColorSpace matrix{AVCOL_SPC_UNSPECIFIED};
  AVColorRange range{AVCOL_RANGE_UNSPECIFIED};
  std::vector<std::int64_t> frame_ticks;
};

struct DecodedAudio final {
  AVCodecID codec_id{AV_CODEC_ID_NONE};
  std::vector<std::int64_t> frame_start_samples;
  std::vector<int> frame_sample_counts;
  std::vector<std::array<std::int16_t, 2>> samples;
};

[[nodiscard]] DecodedVideo decode_video(const std::filesystem::path& path,
                                        const AVRational expected_time_base) {
  AVFormatContext* raw_format = nullptr;
  const std::string name = path_string(path);
  int status = avformat_open_input(&raw_format, name.c_str(), nullptr, nullptr);
  if (status < 0) {
    throw std::runtime_error("avformat_open_input: " + ffmpeg_error(status));
  }
  InputFormatPtr format(raw_format);
  status = avformat_find_stream_info(format.get(), nullptr);
  if (status < 0) {
    throw std::runtime_error("avformat_find_stream_info: " + ffmpeg_error(status));
  }
  const int stream_index =
      av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (stream_index < 0) {
    throw std::runtime_error("av_find_best_stream: " + ffmpeg_error(stream_index));
  }
  AVStream* stream = format->streams[stream_index];
  const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (decoder == nullptr) {
    throw std::runtime_error("exported codec has no decoder");
  }
  CodecContextPtr codec(avcodec_alloc_context3(decoder));
  if (!codec) {
    throw std::runtime_error("avcodec_alloc_context3 failed");
  }
  status = avcodec_parameters_to_context(codec.get(), stream->codecpar);
  if (status < 0 || (status = avcodec_open2(codec.get(), decoder, nullptr)) < 0) {
    throw std::runtime_error("could not open exported video decoder: " + ffmpeg_error(status));
  }
  FramePtr frame(av_frame_alloc());
  PacketPtr packet(av_packet_alloc());
  if (!frame || !packet) {
    throw std::runtime_error("could not allocate decoder buffers");
  }

  DecodedVideo result{.codec_id = stream->codecpar->codec_id,
                      .primaries = stream->codecpar->color_primaries,
                      .transfer = stream->codecpar->color_trc,
                      .matrix = stream->codecpar->color_space,
                      .range = stream->codecpar->color_range,
                      .frame_ticks = {}};
  const auto receive = [&] {
    while (true) {
      const int received = avcodec_receive_frame(codec.get(), frame.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
        return;
      }
      if (received < 0) {
        throw std::runtime_error("avcodec_receive_frame: " + ffmpeg_error(received));
      }
      if (frame->best_effort_timestamp == AV_NOPTS_VALUE) {
        throw std::runtime_error("decoded export frame has no timestamp");
      }
      result.frame_ticks.push_back(
          av_rescale_q_rnd(frame->best_effort_timestamp, stream->time_base, expected_time_base,
                           static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX)));
      av_frame_unref(frame.get());
    }
  };

  while ((status = av_read_frame(format.get(), packet.get())) >= 0) {
    if (packet->stream_index == stream_index) {
      status = avcodec_send_packet(codec.get(), packet.get());
      if (status < 0) {
        throw std::runtime_error("avcodec_send_packet: " + ffmpeg_error(status));
      }
      receive();
    }
    av_packet_unref(packet.get());
  }
  if (status != AVERROR_EOF) {
    throw std::runtime_error("av_read_frame: " + ffmpeg_error(status));
  }
  status = avcodec_send_packet(codec.get(), nullptr);
  if (status < 0) {
    throw std::runtime_error("decoder flush failed: " + ffmpeg_error(status));
  }
  receive();
  return result;
}

[[nodiscard]] DecodedAudio decode_audio(const std::filesystem::path& path) {
  AVFormatContext* raw_format = nullptr;
  const std::string name = path_string(path);
  int status = avformat_open_input(&raw_format, name.c_str(), nullptr, nullptr);
  if (status < 0) {
    throw std::runtime_error("avformat_open_input: " + ffmpeg_error(status));
  }
  InputFormatPtr format(raw_format);
  status = avformat_find_stream_info(format.get(), nullptr);
  if (status < 0) {
    throw std::runtime_error("avformat_find_stream_info: " + ffmpeg_error(status));
  }
  const int stream_index =
      av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (stream_index < 0) {
    throw std::runtime_error("export has no audio stream: " + ffmpeg_error(stream_index));
  }
  AVStream* stream = format->streams[stream_index];
  const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (decoder == nullptr) {
    throw std::runtime_error("exported audio codec has no decoder");
  }
  CodecContextPtr codec(avcodec_alloc_context3(decoder));
  if (!codec) {
    throw std::runtime_error("avcodec_alloc_context3 failed");
  }
  status = avcodec_parameters_to_context(codec.get(), stream->codecpar);
  if (status < 0 || (status = avcodec_open2(codec.get(), decoder, nullptr)) < 0) {
    throw std::runtime_error("could not open exported audio decoder: " + ffmpeg_error(status));
  }
  FramePtr frame(av_frame_alloc());
  PacketPtr packet(av_packet_alloc());
  if (!frame || !packet) {
    throw std::runtime_error("could not allocate audio decoder buffers");
  }

  DecodedAudio result{.codec_id = stream->codecpar->codec_id,
                      .frame_start_samples = {},
                      .frame_sample_counts = {},
                      .samples = {}};
  const auto receive = [&] {
    while (true) {
      const int received = avcodec_receive_frame(codec.get(), frame.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
        return;
      }
      if (received < 0) {
        throw std::runtime_error("avcodec_receive_frame: " + ffmpeg_error(received));
      }
      if (frame->format != AV_SAMPLE_FMT_S16 || frame->ch_layout.nb_channels != 2 ||
          frame->best_effort_timestamp == AV_NOPTS_VALUE) {
        throw std::runtime_error("decoded export audio has an unexpected format or timestamp");
      }
      result.frame_start_samples.push_back(
          av_rescale_q_rnd(frame->best_effort_timestamp, stream->time_base,
                           AVRational{1, static_cast<int>(audio_render::kTimelineAudioSampleRate)},
                           static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX)));
      result.frame_sample_counts.push_back(frame->nb_samples);
      for (int index = 0; index < frame->nb_samples; ++index) {
        const auto offset = static_cast<std::size_t>(index) * 4U;
        result.samples.push_back(
            {static_cast<std::int16_t>(AV_RL16(frame->data[0] + offset)),
             static_cast<std::int16_t>(AV_RL16(frame->data[0] + offset + 2U))});
      }
      av_frame_unref(frame.get());
    }
  };

  while ((status = av_read_frame(format.get(), packet.get())) >= 0) {
    if (packet->stream_index == stream_index) {
      status = avcodec_send_packet(codec.get(), packet.get());
      if (status < 0) {
        throw std::runtime_error("avcodec_send_packet: " + ffmpeg_error(status));
      }
      receive();
    }
    av_packet_unref(packet.get());
  }
  if (status != AVERROR_EOF) {
    throw std::runtime_error("av_read_frame: " + ffmpeg_error(status));
  }
  status = avcodec_send_packet(codec.get(), nullptr);
  if (status < 0) {
    throw std::runtime_error("audio decoder flush failed: " + ffmpeg_error(status));
  }
  receive();
  return result;
}

[[nodiscard]] std::vector<char> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(ExportService, ExportsExactHalfOpenFrameCountAndRec709TimestampsFromOriginals) {
  TestDirectory directory;
  auto timeline = make_timeline();
  const auto destination = directory.path() / "exact.mkv";
  std::vector<ExportProgress> progress;
  const auto outcome = export_video(
      {.snapshot = timeline.snapshot,
       .renderer = timeline.renderer,
       .destination = destination,
       .preset = VideoPreset::Ffv1Matroska,
       .overwrite_existing = false,
       .include_audio = false,
       .cancellation = {},
       .progress = [&progress](const ExportProgress& event) { progress.push_back(event); }});
  ASSERT_TRUE(outcome) << outcome.error().message;
  EXPECT_EQ(outcome.value().frame_count, 4U);
  EXPECT_EQ(outcome.value().source_timeline_duration, edit::Time(7, 60));
  EXPECT_EQ(outcome.value().encoded_video_duration, edit::Time(4, 30));
  EXPECT_TRUE(outcome.value().video_exported);
  EXPECT_FALSE(outcome.value().audio_exported);
  EXPECT_TRUE(outcome.value().audio_codec.empty());
  EXPECT_EQ(outcome.value().audio_sample_count, 0U);
  EXPECT_EQ(outcome.value().encoded_audio_duration, edit::Time{});
  ASSERT_EQ(progress.size(), 4U);
  EXPECT_DOUBLE_EQ(progress.back().fraction, 1.0);

  ASSERT_EQ(timeline.provider->requests.size(), 4U);
  for (std::size_t index = 0; index < timeline.provider->requests.size(); ++index) {
    EXPECT_FALSE(timeline.provider->requests[index].permit_proxy);
    EXPECT_EQ(timeline.provider->requests[index].preferred_width, 4);
    EXPECT_EQ(timeline.provider->requests[index].preferred_height, 2);
    EXPECT_EQ(timeline.provider->requests[index].source_time,
              edit::Time(static_cast<std::int64_t>(index), 30));
  }

  const auto decoded = decode_video(destination, AVRational{1, 30});
  EXPECT_EQ(decoded.codec_id, AV_CODEC_ID_FFV1);
  EXPECT_EQ(decoded.primaries, AVCOL_PRI_BT709);
  EXPECT_EQ(decoded.transfer, AVCOL_TRC_BT709);
  EXPECT_EQ(decoded.matrix, AVCOL_SPC_BT709);
  EXPECT_EQ(decoded.range, AVCOL_RANGE_MPEG);
  EXPECT_EQ(decoded.frame_ticks, (std::vector<std::int64_t>{0, 1, 2, 3}));
}

TEST(ExportService, MuxesExactHalfOpenAudioSamplesFromAuthoritativeOriginal) {
  constexpr std::size_t expected_sample_count = 5'600;
  TestDirectory directory;
  const auto original = directory.path() / "authoritative-original.wav";
  write_constant_pcm_wav(original, expected_sample_count);
  auto timeline = make_timeline(original);
  const auto destination = directory.path() / "audio-exact.mkv";
  const auto outcome = export_video({.snapshot = timeline.snapshot,
                                     .renderer = timeline.renderer,
                                     .audio_renderer = timeline.audio_renderer,
                                     .destination = destination,
                                     .preset = VideoPreset::Ffv1Matroska,
                                     .overwrite_existing = false,
                                     .include_audio = true,
                                     .cancellation = {},
                                     .progress = {}});
  ASSERT_TRUE(outcome) << outcome.error().message;
  EXPECT_TRUE(outcome.value().audio_exported);
  EXPECT_EQ(outcome.value().audio_codec, "PCM signed 16-bit little-endian");
  EXPECT_EQ(outcome.value().audio_sample_count, expected_sample_count);
  EXPECT_EQ(outcome.value().encoded_audio_duration,
            edit::Time(expected_sample_count, audio_render::kTimelineAudioSampleRate));
  EXPECT_EQ(outcome.value().source_timeline_duration,
            edit::Time(expected_sample_count, audio_render::kTimelineAudioSampleRate));

  const auto decoded = decode_audio(destination);
  EXPECT_EQ(decoded.codec_id, AV_CODEC_ID_PCM_S16LE);
  ASSERT_EQ(decoded.samples.size(), expected_sample_count);
  ASSERT_EQ(decoded.frame_start_samples.size(), decoded.frame_sample_counts.size());
  std::int64_t expected_frame_start = 0;
  for (std::size_t index = 0; index < decoded.frame_start_samples.size(); ++index) {
    EXPECT_EQ(decoded.frame_start_samples[index], expected_frame_start);
    expected_frame_start += decoded.frame_sample_counts[index];
  }
  EXPECT_EQ(expected_frame_start, static_cast<std::int64_t>(expected_sample_count));
  for (const auto& sample : decoded.samples) {
    EXPECT_EQ(sample[0], 4'096);
    EXPECT_EQ(sample[1], 0);
  }
}

TEST(ExportService, Ffv1MatroskaOutputIsByteDeterministic) {
  TestDirectory directory;
  const auto original = directory.path() / "deterministic-original.wav";
  write_constant_pcm_wav(original, 5'600U);
  const auto first_path = directory.path() / "first.mkv";
  const auto second_path = directory.path() / "second.mkv";
  auto first = make_timeline(original);
  auto second = first;

  const auto first_outcome = export_video({.snapshot = first.snapshot,
                                           .renderer = first.renderer,
                                           .audio_renderer = first.audio_renderer,
                                           .destination = first_path,
                                           .preset = VideoPreset::Ffv1Matroska,
                                           .overwrite_existing = false,
                                           .include_audio = true,
                                           .cancellation = {},
                                           .progress = {}});
  ASSERT_TRUE(first_outcome) << first_outcome.error().message;
  const auto second_outcome = export_video({.snapshot = second.snapshot,
                                            .renderer = second.renderer,
                                            .audio_renderer = second.audio_renderer,
                                            .destination = second_path,
                                            .preset = VideoPreset::Ffv1Matroska,
                                            .overwrite_existing = false,
                                            .include_audio = true,
                                            .cancellation = {},
                                            .progress = {}});
  ASSERT_TRUE(second_outcome) << second_outcome.error().message;
  EXPECT_EQ(read_bytes(first_path), read_bytes(second_path));
}

TEST(ExportService, ProResMovPresetExportsWhenCompatibleEncoderIsAvailable) {
  const PresetInfo info = preset_info(VideoPreset::ProRes422HqMov);
  EXPECT_EQ(info.container, "QuickTime / MOV");
  EXPECT_FALSE(info.lossless);
  if (!info.available) {
    GTEST_SKIP() << "This FFmpeg runtime has no compatible ProRes 422 HQ encoder";
  }

  TestDirectory directory;
  const auto original = directory.path() / "prores-original.wav";
  write_constant_pcm_wav(original, 5'600U);
  auto timeline = make_timeline(original);
  const auto destination = directory.path() / "intermediate.mov";
  const auto outcome = export_video({.snapshot = timeline.snapshot,
                                     .renderer = timeline.renderer,
                                     .audio_renderer = timeline.audio_renderer,
                                     .destination = destination,
                                     .preset = VideoPreset::ProRes422HqMov,
                                     .overwrite_existing = false,
                                     .include_audio = true,
                                     .cancellation = {},
                                     .progress = {}});
  ASSERT_TRUE(outcome) << outcome.error().message;
  EXPECT_EQ(outcome.value().frame_count, 4U);
  EXPECT_TRUE(outcome.value().audio_exported);
  const auto decoded = decode_video(destination, AVRational{1, 30});
  EXPECT_EQ(decoded.codec_id, AV_CODEC_ID_PRORES);
  EXPECT_EQ(decoded.frame_ticks, (std::vector<std::int64_t>{0, 1, 2, 3}));
  EXPECT_EQ(decoded.primaries, AVCOL_PRI_BT709);
  EXPECT_EQ(decoded.transfer, AVCOL_TRC_BT709);
  EXPECT_EQ(decoded.matrix, AVCOL_SPC_BT709);
  const auto decoded_audio = decode_audio(destination);
  EXPECT_EQ(decoded_audio.codec_id, AV_CODEC_ID_PCM_S16LE);
  EXPECT_EQ(decoded_audio.samples.size(), 5'600U);
}

TEST(ExportService, CancellationRemovesTemporaryFileAndPreservesExistingDestination) {
  TestDirectory directory;
  const auto destination = directory.path() / "existing.mkv";
  {
    std::ofstream existing(destination, std::ios::binary);
    existing << "existing-destination";
  }
  const auto original = read_bytes(destination);
  auto timeline = make_timeline();
  std::stop_source cancellation;
  const auto outcome = export_video({.snapshot = timeline.snapshot,
                                     .renderer = timeline.renderer,
                                     .destination = destination,
                                     .preset = VideoPreset::Ffv1Matroska,
                                     .overwrite_existing = true,
                                     .include_audio = false,
                                     .cancellation = cancellation.get_token(),
                                     .progress = [&cancellation](const ExportProgress& event) {
                                       if (event.completed_frames == 1U) {
                                         cancellation.request_stop();
                                       }
                                     }});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ExportErrorCode::Cancelled);
  EXPECT_EQ(read_bytes(destination), original);
  for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
    EXPECT_FALSE(entry.path().filename().string().starts_with(".existing.mkv.export-"));
  }
}

TEST(ExportService, RejectsInvalidRequestsWithoutCreatingOutput) {
  TestDirectory directory;
  auto timeline = make_timeline();
  const auto no_renderer_path = directory.path() / "no-renderer.mkv";
  const auto no_renderer = export_video({.snapshot = timeline.snapshot,
                                         .renderer = nullptr,
                                         .destination = no_renderer_path,
                                         .preset = VideoPreset::Ffv1Matroska,
                                         .overwrite_existing = false,
                                         .include_audio = false,
                                         .cancellation = {},
                                         .progress = {}});
  ASSERT_FALSE(no_renderer);
  EXPECT_EQ(no_renderer.error().code, ExportErrorCode::InvalidRequest);
  EXPECT_FALSE(std::filesystem::exists(no_renderer_path));

  const auto audio_path = directory.path() / "audio.mkv";
  const auto audio = export_video({.snapshot = timeline.snapshot,
                                   .renderer = timeline.renderer,
                                   .destination = audio_path,
                                   .preset = VideoPreset::Ffv1Matroska,
                                   .overwrite_existing = false,
                                   .include_audio = true,
                                   .cancellation = {},
                                   .progress = {}});
  ASSERT_FALSE(audio);
  EXPECT_EQ(audio.error().code, ExportErrorCode::AudioRendererRequired);
  EXPECT_FALSE(std::filesystem::exists(audio_path));
}

TEST(ExportService, PreservesTypedAudioRenderFailureAndExistingDestination) {
  TestDirectory directory;
  const auto original = directory.path() / "missing-registration.wav";
  write_constant_pcm_wav(original, 5'600U);
  auto empty_registry = std::make_shared<audio_render::OriginalAudioRegistry>();
  auto timeline = make_timeline(std::nullopt, empty_registry);
  const auto destination = directory.path() / "typed-failure.mkv";
  {
    std::ofstream existing(destination, std::ios::binary);
    existing << "existing-destination";
  }
  const auto original_destination = read_bytes(destination);
  const auto outcome = export_video({.snapshot = timeline.snapshot,
                                     .renderer = timeline.renderer,
                                     .audio_renderer = timeline.audio_renderer,
                                     .destination = destination,
                                     .preset = VideoPreset::Ffv1Matroska,
                                     .overwrite_existing = true,
                                     .include_audio = true,
                                     .cancellation = {},
                                     .progress = {}});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ExportErrorCode::AudioRenderFailed);
  ASSERT_TRUE(outcome.error().audio_render_error.has_value());
  EXPECT_EQ(outcome.error().audio_render_error->code,
            audio_render::AudioRenderErrorCode::MissingMedia);
  EXPECT_EQ(outcome.error().audio_render_error->asset_id, timeline.asset_id);
  EXPECT_EQ(read_bytes(destination), original_destination);
}

TEST(ExportService, AudioRendererCancellationRemovesTemporaryAndPreservesDestination) {
  TestDirectory directory;
  const auto original = directory.path() / "cancel-original.wav";
  write_constant_pcm_wav(original, 5'600U);
  std::stop_source cancellation;
  auto cancelling_provider = std::make_shared<CancellingOriginalProvider>(original, cancellation);
  auto timeline = make_timeline(std::nullopt, std::move(cancelling_provider));
  const auto destination = directory.path() / "audio-cancel.mkv";
  {
    std::ofstream existing(destination, std::ios::binary);
    existing << "existing-destination";
  }
  const auto original_destination = read_bytes(destination);
  const auto outcome = export_video({.snapshot = timeline.snapshot,
                                     .renderer = timeline.renderer,
                                     .audio_renderer = timeline.audio_renderer,
                                     .destination = destination,
                                     .preset = VideoPreset::Ffv1Matroska,
                                     .overwrite_existing = true,
                                     .include_audio = true,
                                     .cancellation = cancellation.get_token(),
                                     .progress = {}});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ExportErrorCode::Cancelled);
  ASSERT_TRUE(outcome.error().audio_render_error.has_value());
  EXPECT_EQ(outcome.error().audio_render_error->code,
            audio_render::AudioRenderErrorCode::Cancelled);
  EXPECT_EQ(read_bytes(destination), original_destination);
  for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
    EXPECT_FALSE(entry.path().filename().string().starts_with(".audio-cancel.mkv.export-"));
  }
}

TEST(ExportService, DoesNotOverwriteExistingDestinationWithoutPermission) {
  TestDirectory directory;
  auto timeline = make_timeline();
  const auto destination = directory.path() / "protected.mkv";
  {
    std::ofstream existing(destination, std::ios::binary);
    existing << "protected";
  }
  const auto original = read_bytes(destination);
  const auto outcome = export_video({.snapshot = timeline.snapshot,
                                     .renderer = timeline.renderer,
                                     .destination = destination,
                                     .preset = VideoPreset::Ffv1Matroska,
                                     .overwrite_existing = false,
                                     .include_audio = false,
                                     .cancellation = {},
                                     .progress = {}});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().code, ExportErrorCode::DestinationExists);
  EXPECT_EQ(read_bytes(destination), original);
}

} // namespace
} // namespace video_editor::export_service
