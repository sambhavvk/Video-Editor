// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/export_service.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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

struct TestTimeline final {
  edit::TimelineSnapshot snapshot;
  std::shared_ptr<DeterministicProvider> provider;
  std::shared_ptr<render::CpuRenderer> renderer;
};

[[nodiscard]] TestTimeline make_timeline() {
  edit::Project project;
  edit::Asset asset;
  asset.name = "Generated source";
  asset.source_uri = "memory://generated";
  asset.duration = edit::Time(7, 60);
  asset.has_video = true;
  asset.width = 4;
  asset.height = 2;
  asset.nominal_frame_rate = edit::Rate(30, 1);

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
  return {.snapshot = std::move(snapshot).value(),
          .provider = std::move(provider),
          .renderer = std::move(renderer)};
}

struct DecodedVideo final {
  AVCodecID codec_id{AV_CODEC_ID_NONE};
  AVColorPrimaries primaries{AVCOL_PRI_UNSPECIFIED};
  AVColorTransferCharacteristic transfer{AVCOL_TRC_UNSPECIFIED};
  AVColorSpace matrix{AVCOL_SPC_UNSPECIFIED};
  AVColorRange range{AVCOL_RANGE_UNSPECIFIED};
  std::vector<std::int64_t> frame_ticks;
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

TEST(ExportService, Ffv1MatroskaOutputIsByteDeterministic) {
  TestDirectory directory;
  const auto first_path = directory.path() / "first.mkv";
  const auto second_path = directory.path() / "second.mkv";
  auto first = make_timeline();
  auto second = first;

  const auto first_outcome = export_video({.snapshot = first.snapshot,
                                           .renderer = first.renderer,
                                           .destination = first_path,
                                           .preset = VideoPreset::Ffv1Matroska,
                                           .overwrite_existing = false,
                                           .include_audio = false,
                                           .cancellation = {},
                                           .progress = {}});
  ASSERT_TRUE(first_outcome) << first_outcome.error().message;
  const auto second_outcome = export_video({.snapshot = second.snapshot,
                                            .renderer = second.renderer,
                                            .destination = second_path,
                                            .preset = VideoPreset::Ffv1Matroska,
                                            .overwrite_existing = false,
                                            .include_audio = false,
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
  auto timeline = make_timeline();
  const auto destination = directory.path() / "intermediate.mov";
  const auto outcome = export_video({.snapshot = timeline.snapshot,
                                     .renderer = timeline.renderer,
                                     .destination = destination,
                                     .preset = VideoPreset::ProRes422HqMov,
                                     .overwrite_existing = false,
                                     .include_audio = false,
                                     .cancellation = {},
                                     .progress = {}});
  ASSERT_TRUE(outcome) << outcome.error().message;
  EXPECT_EQ(outcome.value().frame_count, 4U);
  EXPECT_FALSE(outcome.value().audio_exported);
  const auto decoded = decode_video(destination, AVRational{1, 30});
  EXPECT_EQ(decoded.codec_id, AV_CODEC_ID_PRORES);
  EXPECT_EQ(decoded.frame_ticks, (std::vector<std::int64_t>{0, 1, 2, 3}));
  EXPECT_EQ(decoded.primaries, AVCOL_PRI_BT709);
  EXPECT_EQ(decoded.transfer, AVCOL_TRC_BT709);
  EXPECT_EQ(decoded.matrix, AVCOL_SPC_BT709);
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

TEST(ExportService, RejectsInvalidAndUnsupportedRequestsWithoutCreatingOutput) {
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
  EXPECT_EQ(audio.error().code, ExportErrorCode::AudioNotSupported);
  EXPECT_FALSE(std::filesystem::exists(audio_path));
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
