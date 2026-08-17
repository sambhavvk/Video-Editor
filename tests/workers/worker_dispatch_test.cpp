// SPDX-License-Identifier: MPL-2.0
#include "video_editor/workers/job_dispatch.h"

#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/timeline_editor.h"
#include "video_editor/export_service/export_service.h"
#include "video_editor/job_service/framing.h"
#include "video_editor/job_service/job_id.h"
#include "video_editor/project_codec/project_codec.h"
#include "video_editor/proxy_service/proxy_service.h"
#include "video_editor/transcription_service/transcription_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace video_editor::workers {
namespace {

namespace protocol = jobs::v1;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("video_editor_worker_test_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(path_);
  }

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
#ifndef VIDEO_EDITOR_WORKER_TEST_FFMPEG
  static_cast<void>(path);
  return false;
#else
  const std::vector<std::string> arguments{
      VIDEO_EDITOR_WORKER_TEST_FFMPEG,
      "-hide_banner",
      "-loglevel",
      "error",
      "-nostdin",
      "-y",
      "-f",
      "lavfi",
      "-i",
      "testsrc2=size=160x90:rate=10:duration=0.4",
      "-f",
      "lavfi",
      "-i",
      "sine=frequency=440:sample_rate=48000:duration=0.4",
      "-map",
      "0:v",
      "-map",
      "1:a",
      "-c:v",
      "mpeg4",
      "-q:v",
      "4",
      "-c:a",
      "pcm_s16le",
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

[[nodiscard]] std::string utf8_string(const std::filesystem::path& path) {
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] protocol::JobSpec valid_proxy_spec(const std::filesystem::path& source,
                                                 const std::filesystem::path& destination) {
  protocol::JobSpec spec;
  spec.set_job_id(jobs::make_job_id());
  spec.set_kind(protocol::JOB_KIND_PROXY);
  spec.add_input_uris(utf8_string(source));
  spec.set_output_uri(utf8_string(destination));
  spec.set_preset_id("video-editor.proxy.ffv1-half.v1");
  return spec;
}

[[nodiscard]] protocol::JobSpec valid_transcribe_spec(const std::filesystem::path& source) {
  protocol::JobSpec spec;
  spec.set_job_id(jobs::make_job_id());
  spec.set_kind(protocol::JOB_KIND_TRANSCRIBE);
  spec.add_input_uris(utf8_string(source));
  spec.set_preset_id("video-editor.transcribe.whisper-base.v1");
  protocol::TranscribeOptions options;
  options.set_schema_version(transcription::kTranscriptionSchemaVersion);
  options.set_model_id("base");
  options.set_language("auto");
  EXPECT_TRUE(options.SerializeToString(spec.mutable_options()));
  return spec;
}

TEST(WorkerProxyDispatch, GeneratesProxyAndMonotonicVersionedEvents) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.mkv";
  if (!create_fixture(source)) {
    GTEST_SKIP() << "ffmpeg fixture generator is unavailable";
  }
  const auto destination = directory.path() / "proxy.mkv";
  const protocol::JobSpec spec = valid_proxy_spec(source, destination);

  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(dispatch_job(spec, [&events](const protocol::WorkerEvent& event) {
    events.push_back(event);
    return true;
  }));

  ASSERT_GE(events.size(), 3U);
  EXPECT_EQ(events.front().event().state(), protocol::JOB_STATE_ACCEPTED);
  EXPECT_EQ(events.back().event().state(), protocol::JOB_STATE_SUCCEEDED);
  EXPECT_EQ(events.back().event().result_uri(), utf8_string(destination));
  EXPECT_EQ(events.back().event().metadata().at("contract_version"), "1");
  EXPECT_EQ(events.back().event().metadata().at("video_codec"), "ffv1");
  EXPECT_EQ(events.back().event().metadata().at("container"), "matroska");
  EXPECT_EQ(events.back().event().metadata().at("used_fallback"), "false");
  EXPECT_TRUE(std::filesystem::is_regular_file(destination));
  const auto map_path = proxy::default_pts_map_path(destination);
  EXPECT_EQ(events.back().event().metadata().at("pts_map_uri"), utf8_string(map_path));
  EXPECT_TRUE(std::filesystem::is_regular_file(map_path));
  EXPECT_TRUE(proxy::load_pts_map(map_path));

  double previous = -1.0;
  for (const auto& event : events) {
    EXPECT_EQ(event.protocol_major(), jobs::kProtocolMajor);
    EXPECT_EQ(event.protocol_minor(), jobs::kProtocolMinor);
    EXPECT_EQ(event.event().job_id(), spec.job_id());
    EXPECT_GE(event.event().progress(), previous);
    previous = event.event().progress();
  }
}

TEST(WorkerProxyDispatch, RejectsUnknownOptionsWithoutTouchingDestination) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.mkv";
  if (!create_fixture(source)) {
    GTEST_SKIP() << "ffmpeg fixture generator is unavailable";
  }
  const auto destination = directory.path() / "proxy.mkv";
  protocol::JobSpec spec = valid_proxy_spec(source, destination);
  spec.set_options("future-option=true");

  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(dispatch_job(spec, [&events](const protocol::WorkerEvent& event) {
    events.push_back(event);
    return true;
  }));

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events.back().event().state(), protocol::JOB_STATE_FAILED);
  EXPECT_EQ(events.back().event().phase(), "validating");
  EXPECT_EQ(events.back().event().error().category(), "invalid-argument");
  EXPECT_FALSE(std::filesystem::exists(destination));
  EXPECT_FALSE(std::filesystem::exists(proxy::default_pts_map_path(destination)));
}

TEST(WorkerProxyDispatch, RejectsInvalidUtf8PathsBeforeFilesystemAccess) {
  TemporaryDirectory directory;
  const auto destination = directory.path() / "proxy.mkv";
  std::string invalid_source = utf8_string(directory.path() / "source.mkv");
  invalid_source.push_back(static_cast<char>(0xFF));

  protocol::JobSpec spec;
  spec.set_job_id(jobs::make_job_id());
  spec.set_kind(protocol::JOB_KIND_PROXY);
  spec.add_input_uris(invalid_source);
  spec.set_output_uri(utf8_string(destination));
  spec.set_preset_id("video-editor.proxy.ffv1-half.v1");

  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(dispatch_job(spec, [&events](const protocol::WorkerEvent& event) {
    events.push_back(event);
    return true;
  }));

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events.back().event().state(), protocol::JOB_STATE_FAILED);
  EXPECT_EQ(events.back().event().phase(), "validating");
  EXPECT_EQ(events.back().event().error().category(), "invalid-argument");
  EXPECT_NE(events.back().event().error().diagnostic().find("UTF-8"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST(WorkerProxyDispatch, ReportsMediaFailureAsTerminalEvent) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "corrupt.mkv";
  {
    std::ofstream output(source, std::ios::binary | std::ios::trunc);
    output << "not media";
  }
  const auto destination = directory.path() / "proxy.mkv";
  const protocol::JobSpec spec = valid_proxy_spec(source, destination);

  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(dispatch_job(spec, [&events](const protocol::WorkerEvent& event) {
    events.push_back(event);
    return true;
  }));

  ASSERT_GE(events.size(), 2U);
  EXPECT_EQ(events.back().event().state(), protocol::JOB_STATE_FAILED);
  EXPECT_EQ(events.back().event().phase(), "failed");
  EXPECT_EQ(events.back().event().error().category(), "media-open");
  EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST(WorkerProxyDispatch, MakesSynchronousCancellationLimitationExplicit) {
  protocol::CancelJob request;
  request.set_job_id(jobs::make_job_id());
  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(
      reject_unavailable_cancellation(request, [&events](const protocol::WorkerEvent& event) {
        events.push_back(event);
        return true;
      }));
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front().event().state(), protocol::JOB_STATE_FAILED);
  EXPECT_EQ(events.front().event().error().category(), "unsupported-cancellation");
}

TEST(WorkerTranscriptionDispatch, ReportsTypedBackendUnavailableWithoutBackend) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.wav";
  std::ofstream(source, std::ios::binary) << "fixture";
  const protocol::JobSpec spec = valid_transcribe_spec(source);
  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(dispatch_job(spec, [&events](const protocol::WorkerEvent& event) {
    events.push_back(event);
    return true;
  }));
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events.back().event().state(), protocol::JOB_STATE_FAILED);
  EXPECT_EQ(events.back().event().error().category(), "backend-unavailable");
}

TEST(WorkerTranscriptionDispatch, RejectsMalformedTypedOptionsBeforeInputIo) {
  TemporaryDirectory directory;
  protocol::JobSpec spec = valid_transcribe_spec(directory.path() / "missing.wav");
  spec.set_options("not-a-protobuf");
  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(dispatch_job(spec, [&events](const protocol::WorkerEvent& event) {
    events.push_back(event);
    return true;
  }));
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events.back().event().error().category(), "invalid-argument");
}

TEST(WorkerHost, FramedTranscriptionRequestReportsBackendUnavailable) {
#ifndef VIDEO_EDITOR_WORKER_HOST
  GTEST_SKIP() << "worker host path is not configured";
#else
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.wav";
  std::ofstream(source, std::ios::binary) << "fixture";
  const auto input = directory.path() / "request.bin";
  const auto output = directory.path() / "events.bin";
  {
    std::ofstream stream(input, std::ios::binary | std::ios::trunc);
    protocol::WorkerRequest request;
    request.set_protocol_major(jobs::kProtocolMajor);
    request.set_protocol_minor(jobs::kProtocolMinor);
    *request.mutable_start()->mutable_spec() = valid_transcribe_spec(source);
    ASSERT_TRUE(jobs::write_frame(stream, request).ok);
  }
  const std::string command = shell_quote(VIDEO_EDITOR_WORKER_HOST) + " < " +
                              shell_quote(input.string()) + " > " + shell_quote(output.string());
  ASSERT_EQ(std::system(command.c_str()), 0);

  std::ifstream stream(output, std::ios::binary);
  ASSERT_TRUE(stream);
  std::vector<protocol::WorkerEvent> events;
  while (true) {
    protocol::WorkerEvent event;
    const auto result = jobs::read_frame(stream, event);
    if (result.status == jobs::ReadStatus::EndOfStream)
      break;
    ASSERT_TRUE(result.ok) << result.message;
    events.push_back(std::move(event));
  }
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events.front().event().state(), protocol::JOB_STATE_ACCEPTED);
  EXPECT_EQ(events.back().event().state(), protocol::JOB_STATE_FAILED);
  EXPECT_EQ(events.back().event().error().category(), "backend-unavailable");
#endif
}

[[nodiscard]] bool write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

[[nodiscard]] protocol::JobSpec valid_export_spec(const std::filesystem::path& checkpoint,
                                                  const std::filesystem::path& destination,
                                                  const std::string& sequence_id) {
  protocol::JobSpec spec;
  spec.set_job_id(jobs::make_job_id());
  spec.set_kind(protocol::JOB_KIND_EXPORT);
  spec.set_project_checkpoint(utf8_string(checkpoint));
  spec.set_output_uri(utf8_string(destination));
  spec.set_preset_id("video-editor.export.creator.v1");
  protocol::ExportOptions options;
  options.set_schema_version(1);
  options.set_platform_preset(
      static_cast<int>(export_service::PlatformPreset::ReferenceFfv1));
  options.set_caption_mode("none");
  options.set_sidecar_format("srt");
  options.set_include_audio(true);
  options.set_overwrite_existing(true);
  options.set_prefer_hardware(false);
  options.set_sequence_id(sequence_id);
  EXPECT_TRUE(options.SerializeToString(spec.mutable_options()));
  return spec;
}

[[nodiscard]] bool write_one_clip_checkpoint(const std::filesystem::path& source,
                                             const std::filesystem::path& checkpoint,
                                             std::string& sequence_id) {
  edit::Project project;
  edit::Asset asset;
  asset.name = "fixture";
  asset.source_uri = utf8_string(source);
  asset.duration = edit::Time(2, 5);
  asset.has_video = true;
  asset.has_audio = true;
  asset.width = 160;
  asset.height = 90;
  asset.nominal_frame_rate = edit::Rate(10, 1);
  asset.audio_sample_rate = 48'000;
  asset.audio_channels = 2;

  edit::Clip clip;
  clip.asset_id = asset.id;
  clip.timeline_range = {edit::Time{}, asset.duration};
  clip.source_range = clip.timeline_range;

  edit::Track video_track;
  video_track.kind = edit::TrackKind::Video;
  video_track.clips.push_back(clip);

  edit::Clip audio_clip = clip;
  audio_clip.id = edit::EntityId::generate();
  audio_clip.kind = edit::ClipKind::Audio;
  edit::Track audio_track;
  audio_track.kind = edit::TrackKind::Audio;
  audio_track.clips.push_back(std::move(audio_clip));

  edit::Sequence sequence;
  sequence.name = "Export fixture";
  sequence.width = 160;
  sequence.height = 90;
  sequence.frame_rate = edit::Rate(10, 1);
  sequence.tracks.push_back(std::move(video_track));
  sequence.tracks.push_back(std::move(audio_track));
  sequence_id = sequence.id.toString();

  project.assets.push_back(std::move(asset));
  project.sequences.push_back(std::move(sequence));
  try {
    return write_bytes(checkpoint, project_codec::serialize_project(project));
  } catch (...) {
    return false;
  }
}

TEST(WorkerExportDispatch, ExportsTinyLavfiProjectSnapshot) {
  if (!export_service::preset_info(export_service::VideoPreset::Ffv1Matroska).available) {
    GTEST_SKIP() << "FFV1 encoder is unavailable";
  }
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.mkv";
  if (!create_fixture(source)) {
    GTEST_SKIP() << "ffmpeg fixture generator is unavailable";
  }
  const auto checkpoint = directory.path() / "project.veproj";
  std::string sequence_id;
  ASSERT_TRUE(write_one_clip_checkpoint(source, checkpoint, sequence_id));
  const auto destination = directory.path() / "export.mkv";
  const protocol::JobSpec spec = valid_export_spec(checkpoint, destination, sequence_id);

  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(dispatch_job(spec, [&events](const protocol::WorkerEvent& event) {
    events.push_back(event);
    return true;
  }));

  ASSERT_GE(events.size(), 2U);
  EXPECT_EQ(events.front().event().state(), protocol::JOB_STATE_ACCEPTED);
  ASSERT_EQ(events.back().event().state(), protocol::JOB_STATE_SUCCEEDED)
      << events.back().event().error().category() << ": "
      << events.back().event().error().user_message() << " / "
      << events.back().event().error().diagnostic();
  EXPECT_EQ(events.back().event().result_uri(), utf8_string(destination));
  EXPECT_EQ(events.back().event().metadata().at("contract_version"), "1");
  EXPECT_FALSE(events.back().event().metadata().at("frame_count").empty());
  EXPECT_TRUE(std::filesystem::is_regular_file(destination));
}

TEST(WorkerExportDispatch, RejectsUnknownOptionsWithoutTouchingDestination) {
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.mkv";
  if (!create_fixture(source)) {
    GTEST_SKIP() << "ffmpeg fixture generator is unavailable";
  }
  const auto checkpoint = directory.path() / "project.veproj";
  std::string sequence_id;
  ASSERT_TRUE(write_one_clip_checkpoint(source, checkpoint, sequence_id));
  const auto destination = directory.path() / "export.mkv";
  protocol::JobSpec spec = valid_export_spec(checkpoint, destination, sequence_id);
  spec.set_options("not-a-protobuf");

  std::vector<protocol::WorkerEvent> events;
  ASSERT_TRUE(dispatch_job(spec, [&events](const protocol::WorkerEvent& event) {
    events.push_back(event);
    return true;
  }));
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events.back().event().state(), protocol::JOB_STATE_FAILED);
  EXPECT_EQ(events.back().event().error().category(), "invalid-argument");
  EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST(WorkerHost, FramedProxyRequestGeneratesProxy) {
#ifndef VIDEO_EDITOR_WORKER_HOST
  GTEST_SKIP() << "worker host path is not configured";
#else
  TemporaryDirectory directory;
  const auto source = directory.path() / "source.mkv";
  if (!create_fixture(source)) {
    GTEST_SKIP() << "ffmpeg fixture generator is unavailable";
  }
  const auto destination = directory.path() / "proxy.mkv";
  const auto input = directory.path() / "request.bin";
  const auto output = directory.path() / "events.bin";
  {
    std::ofstream stream(input, std::ios::binary | std::ios::trunc);
    protocol::WorkerRequest request;
    request.set_protocol_major(jobs::kProtocolMajor);
    request.set_protocol_minor(jobs::kProtocolMinor);
    *request.mutable_start()->mutable_spec() = valid_proxy_spec(source, destination);
    ASSERT_TRUE(jobs::write_frame(stream, request).ok);
  }
  const std::string command = shell_quote(VIDEO_EDITOR_WORKER_HOST) + " < " +
                              shell_quote(input.string()) + " > " + shell_quote(output.string());
  ASSERT_EQ(std::system(command.c_str()), 0);

  std::ifstream stream(output, std::ios::binary);
  ASSERT_TRUE(stream);
  std::vector<protocol::WorkerEvent> events;
  while (true) {
    protocol::WorkerEvent event;
    const auto result = jobs::read_frame(stream, event);
    if (result.status == jobs::ReadStatus::EndOfStream)
      break;
    ASSERT_TRUE(result.ok) << result.message;
    events.push_back(std::move(event));
  }
  ASSERT_GE(events.size(), 2U);
  EXPECT_EQ(events.front().event().state(), protocol::JOB_STATE_ACCEPTED);
  EXPECT_EQ(events.back().event().state(), protocol::JOB_STATE_SUCCEEDED);
  EXPECT_TRUE(std::filesystem::is_regular_file(destination));
#endif
}

} // namespace
} // namespace video_editor::workers
