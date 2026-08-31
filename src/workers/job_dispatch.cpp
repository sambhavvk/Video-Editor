// SPDX-License-Identifier: MPL-2.0
#include "video_editor/workers/job_dispatch.h"

#include "video_editor/job_service/cancellation_registry.h"
#include "video_editor/audio_render/original_audio_registry.h"
#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/timeline_editor.h"
#include "video_editor/export_service/export_service.h"
#include "video_editor/job_service/job_id.h"
#include "video_editor/media_cache/cache_store.h"
#include "video_editor/media_cache/thumbnail_service.h"
#include "video_editor/media_cache/waveform_service.h"
#include "video_editor/media_codec/probe.h"
#include "video_editor/playback/asset_registry.h"
#include "video_editor/playback/ffmpeg_frame_provider.h"
#include "video_editor/project_codec/project_codec.h"
#include "video_editor/proxy_service/proxy_service.h"
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/transcription_service/transcription_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace video_editor::workers {
namespace {

namespace transcription = video_editor::transcription;

using jobs::kProtocolMajor;
using jobs::kProtocolMinor;
namespace protocol = jobs::v1;

constexpr std::string_view kProResHalfPreset{"video-editor.proxy.prores-half.v1"};
constexpr std::string_view kFfv1HalfPreset{"video-editor.proxy.ffv1-half.v1"};
constexpr std::string_view kTranscribePreset{"video-editor.transcribe.whisper-base.v1"};
constexpr std::string_view kExportPreset{"video-editor.export.creator.v1"};
constexpr std::string_view kThumbnailPreset{"video-editor.thumbnail.v1"};
constexpr std::string_view kWaveformPreset{"video-editor.waveform.v1"};
constexpr std::uint32_t kExportSchemaVersion = 1;
constexpr std::uint32_t kThumbnailJobSchemaVersion = 1;
constexpr std::uint32_t kWaveformJobSchemaVersion = 1;

class RegistryCancelWatcher final {
public:
  RegistryCancelWatcher(const jobs::CancellationRegistry::Token& token,
                        std::stop_source& internal_stop)
      : token_(token), internal_stop_(&internal_stop),
        thread_([this](const std::stop_token stop) { watch(stop); }) {}

  ~RegistryCancelWatcher() {
    thread_.request_stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  RegistryCancelWatcher(const RegistryCancelWatcher&) = delete;
  RegistryCancelWatcher& operator=(const RegistryCancelWatcher&) = delete;

private:
  void watch(const std::stop_token stop) {
    while (!stop.stop_requested()) {
      if (token_->load(std::memory_order_acquire)) {
        internal_stop_->request_stop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  jobs::CancellationRegistry::Token token_;
  std::stop_source* internal_stop_;
  std::jthread thread_;
};

protocol::WorkerEvent event_for(const protocol::JobSpec& spec, const protocol::JobState state,
                                const double progress, const std::string_view phase) {
  protocol::WorkerEvent envelope;
  envelope.set_protocol_major(kProtocolMajor);
  envelope.set_protocol_minor(kProtocolMinor);
  protocol::JobEvent* event = envelope.mutable_event();
  event->set_job_id(spec.job_id());
  event->set_state(state);
  event->set_progress(progress);
  event->set_phase(phase);
  return envelope;
}

void fail(protocol::WorkerEvent& event, const std::string_view category, const int native_code,
          const std::string_view user_message, const std::string_view diagnostic,
          const bool retryable = false) {
  event.mutable_event()->set_state(protocol::JOB_STATE_FAILED);
  protocol::JobError* error = event.mutable_event()->mutable_error();
  error->set_category(category);
  error->set_native_code(native_code);
  error->set_user_message(user_message);
  error->set_diagnostic(diagnostic);
  error->set_retryable(retryable);
}

[[nodiscard]] bool has_embedded_nul(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

[[nodiscard]] bool valid_utf8(const std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = static_cast<std::uint8_t>(static_cast<unsigned char>(value[index]));
    if (lead <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      continuation_count = 1;
      codepoint = static_cast<std::uint32_t>(lead & 0x1FU);
      minimum = 0x80U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation_count = 2;
      codepoint = static_cast<std::uint32_t>(lead & 0x0FU);
      minimum = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation_count = 3;
      codepoint = static_cast<std::uint32_t>(lead & 0x07U);
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > value.size() - index - 1U) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto byte =
          static_cast<std::uint8_t>(static_cast<unsigned char>(value[index + offset]));
      if ((byte & 0xC0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | static_cast<std::uint32_t>(byte & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

[[nodiscard]] std::filesystem::path utf8_path(const std::string& value) {
  std::u8string encoded;
  encoded.reserve(value.size());
  for (const char byte : value) {
    encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
  }
  return std::filesystem::path(encoded);
}

[[nodiscard]] std::string utf8_string(const std::filesystem::path& path) {
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] bool emit_invalid(const protocol::JobSpec& spec, const EventSink& sink,
                                const std::string_view diagnostic) {
  auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "validating");
  fail(event, "invalid-argument", 0, "The proxy job settings are not valid.", diagnostic);
  return sink(event);
}

[[nodiscard]] bool run_probe(const protocol::JobSpec& spec, const EventSink& sink) {
  if (spec.input_uris_size() != 1) {
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "validating");
    fail(event, "invalid-argument", 0, "The probe job needs one media file.",
         "input_uris must contain exactly one item");
    return sink(event);
  }
  if (!sink(event_for(spec, protocol::JOB_STATE_RUNNING, 0.1, "probing"))) {
    return false;
  }

  const auto result = media::probe(spec.input_uris(0));
  auto event = event_for(spec, protocol::JOB_STATE_SUCCEEDED, 1.0, "complete");
  if (!result) {
    fail(event, "media-probe", result.error().native_code, "The media file could not be read.",
         result.error().message);
  } else {
    const auto& asset = result.value();
    auto* metadata = event.mutable_event()->mutable_metadata();
    (*metadata)["format"] = asset.format_name;
    (*metadata)["streams"] = std::to_string(asset.streams.size());
    (*metadata)["best_video_stream"] = std::to_string(asset.best_video_stream);
    (*metadata)["best_audio_stream"] = std::to_string(asset.best_audio_stream);
    if (asset.duration_microseconds.has_value()) {
      (*metadata)["duration_us"] = std::to_string(*asset.duration_microseconds);
    }
  }
  return sink(event);
}

struct ParsedProxyRequest {
  proxy::GenerateRequest request;
  std::string preset_id;
};

[[nodiscard]] bool parse_proxy_request(const protocol::JobSpec& spec, ParsedProxyRequest& parsed,
                                       std::string& error) {
  if (!jobs::valid_job_id(spec.job_id())) {
    error = "job_id must be a canonical UUID job identifier";
    return false;
  }
  if (spec.input_uris_size() != 1) {
    error = "input_uris must contain exactly one absolute local filesystem path";
    return false;
  }
  if (spec.input_uris(0).empty() || spec.output_uri().empty()) {
    error = "source and output paths must not be empty";
    return false;
  }
  if (has_embedded_nul(spec.input_uris(0)) || has_embedded_nul(spec.output_uri())) {
    error = "source and output paths must not contain NUL bytes";
    return false;
  }
  if (!valid_utf8(spec.input_uris(0)) || !valid_utf8(spec.output_uri())) {
    error = "source and output paths must be well-formed UTF-8";
    return false;
  }
  if (!spec.options().empty()) {
    error = "options must be empty for the proxy v1 contract";
    return false;
  }
  if (!spec.project_checkpoint().empty()) {
    error = "project_checkpoint is not used by the proxy v1 contract and must be empty";
    return false;
  }

  const std::filesystem::path source = utf8_path(spec.input_uris(0));
  const std::filesystem::path destination = utf8_path(spec.output_uri());
  if (!source.is_absolute() || !destination.is_absolute()) {
    error = "source and output must be absolute local filesystem paths";
    return false;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(source, filesystem_error)) {
    error = filesystem_error ? "source path could not be inspected: " + filesystem_error.message()
                             : "source path is not a regular file";
    return false;
  }
  filesystem_error.clear();
  const bool destination_exists = std::filesystem::exists(destination, filesystem_error);
  if (filesystem_error) {
    error = "output path could not be inspected: " + filesystem_error.message();
    return false;
  }
  if (destination_exists && std::filesystem::is_directory(destination, filesystem_error)) {
    error = "output path refers to a directory";
    return false;
  }
  if (filesystem_error) {
    error = "output path could not be inspected: " + filesystem_error.message();
    return false;
  }

  proxy::ProxyProfile profile;
  if (spec.preset_id() == kProResHalfPreset) {
    profile = {};
  } else if (spec.preset_id() == kFfv1HalfPreset) {
    profile = proxy::patent_neutral_fallback_profile();
  } else {
    error = "preset_id must be video-editor.proxy.prores-half.v1 or "
            "video-editor.proxy.ffv1-half.v1";
    return false;
  }

  parsed.request = {
      .source = source,
      .destination = destination,
      .pts_map_destination = std::nullopt,
      .profile = profile,
  };
  parsed.preset_id = spec.preset_id();
  return true;
}

[[nodiscard]] std::string_view progress_phase(const proxy::ProgressStage stage) {
  switch (stage) {
  case proxy::ProgressStage::Inspecting:
    return "inspecting";
  case proxy::ProgressStage::Transcoding:
    return "transcoding";
  case proxy::ProgressStage::Finalizing:
    return "finalizing";
  case proxy::ProgressStage::Complete:
    return "finishing";
  }
  return "working";
}

[[nodiscard]] double protocol_progress(const proxy::Progress& progress) {
  switch (progress.stage) {
  case proxy::ProgressStage::Inspecting:
    return 0.05;
  case proxy::ProgressStage::Transcoding:
    return 0.1 + (std::clamp(progress.fraction, 0.0, 1.0) * 0.8);
  case proxy::ProgressStage::Finalizing:
    return 0.95;
  case proxy::ProgressStage::Complete:
    return 0.99;
  }
  return 0.0;
}

struct ProxyErrorDescription {
  std::string_view category;
  std::string_view user_message;
  bool retryable{false};
};

[[nodiscard]] ProxyErrorDescription describe(const proxy::ErrorCode code) {
  switch (code) {
  case proxy::ErrorCode::InvalidArgument:
    return {"invalid-argument", "The proxy job settings are not valid."};
  case proxy::ErrorCode::SourceNotFound:
    return {"source-not-found", "The source media is missing or unreadable."};
  case proxy::ErrorCode::EncoderUnavailable:
    return {"encoder-unavailable", "No supported proxy encoder is available."};
  case proxy::ErrorCode::OpenFailed:
    return {"media-open", "The source media or proxy destination could not be opened."};
  case proxy::ErrorCode::DecodeFailed:
    return {"media-decode", "The source media could not be decoded."};
  case proxy::ErrorCode::EncodeFailed:
    return {"media-encode", "The proxy could not be encoded."};
  case proxy::ErrorCode::WriteFailed:
    return {"io-write", "The proxy could not be saved.", true};
  case proxy::ErrorCode::InvalidPtsMap:
    return {"invalid-pts-map", "The proxy timestamp map is invalid."};
  case proxy::ErrorCode::Cancelled:
    return {"cancelled", "Proxy generation was cancelled."};
  case proxy::ErrorCode::Internal:
  case proxy::ErrorCode::None:
    return {"internal", "Proxy generation failed unexpectedly."};
  }
  return {"internal", "Proxy generation failed unexpectedly."};
}

[[nodiscard]] bool run_proxy(const protocol::JobSpec& spec, const EventSink& sink,
                             const jobs::CancellationRegistry::Token& cancel_token) {
  ParsedProxyRequest parsed;
  std::string validation_error;
  if (!parse_proxy_request(spec, parsed, validation_error)) {
    return emit_invalid(spec, sink, validation_error);
  }

  std::stop_source internal_stop;
  RegistryCancelWatcher cancel_watcher(cancel_token, internal_stop);
  double last_progress = 0.0;
  bool sink_available = true;
  const auto progress = [&](const proxy::Progress& update) {
    if (!sink_available) {
      return;
    }
    if (cancel_token->load(std::memory_order_acquire)) {
      internal_stop.request_stop();
    }
    const double next = std::max(last_progress, protocol_progress(update));
    auto event = event_for(spec, protocol::JOB_STATE_RUNNING, next, progress_phase(update.stage));
    auto* metadata = event.mutable_event()->mutable_metadata();
    (*metadata)["video_frames"] = std::to_string(update.video_frames);
    (*metadata)["audio_samples"] = std::to_string(update.audio_samples);
    sink_available = sink(event);
    last_progress = next;
    if (!sink_available) {
      internal_stop.request_stop();
    }
  };

  const auto generated = proxy::generate_proxy(parsed.request, internal_stop.get_token(), progress);
  if (!sink_available) {
    return false;
  }
  if (!generated) {
    const ProxyErrorDescription description = describe(generated.error().code);
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, last_progress, "failed");
    fail(event, description.category, generated.error().native_code, description.user_message,
         generated.error().message, description.retryable);
    if (generated.error().code == proxy::ErrorCode::Cancelled) {
      event.mutable_event()->set_state(protocol::JOB_STATE_CANCELLED);
      event.mutable_event()->set_phase("cancelled");
    }
    return sink(event);
  }

  const proxy::GenerateResult& result = generated.value();
  auto event = event_for(spec, protocol::JOB_STATE_SUCCEEDED, 1.0, "complete");
  protocol::JobEvent* job_event = event.mutable_event();
  job_event->set_result_uri(utf8_string(result.destination));
  auto* metadata = job_event->mutable_metadata();
  (*metadata)["contract_version"] = "1";
  (*metadata)["preset_id"] = parsed.preset_id;
  (*metadata)["pts_map_uri"] = utf8_string(result.pts_map_path);
  (*metadata)["video_codec"] =
      result.profile.video_codec == proxy::VideoCodec::ProResProxy ? "prores-proxy" : "ffv1";
  (*metadata)["container"] =
      result.profile.container == proxy::Container::QuickTime ? "mov" : "matroska";
  (*metadata)["used_fallback"] = result.profile.used_fallback ? "true" : "false";
  (*metadata)["width"] = std::to_string(result.width);
  (*metadata)["height"] = std::to_string(result.height);
  (*metadata)["audio_included"] = result.audio_included ? "true" : "false";
  (*metadata)["video_frames"] = std::to_string(result.video_frames);
  (*metadata)["audio_samples"] = std::to_string(result.audio_samples);
  return sink(event);
}

struct ParsedTranscribeRequest final {
  std::filesystem::path input;
  transcription::OptionsMessage options;
};

[[nodiscard]] bool parse_transcribe_request(const protocol::JobSpec& spec,
                                            ParsedTranscribeRequest& parsed, std::string& error) {
  if (!jobs::valid_job_id(spec.job_id())) {
    error = "job_id must be a canonical UUID job identifier";
    return false;
  }
  if (spec.input_uris_size() != 1 || spec.input_uris(0).empty()) {
    error = "input_uris must contain exactly one audio/video file";
    return false;
  }
  if (spec.output_uri().size() != 0U || !spec.project_checkpoint().empty()) {
    error = "output_uri and project_checkpoint must be empty for transcription";
    return false;
  }
  if (spec.preset_id() != kTranscribePreset) {
    error = "preset_id must be video-editor.transcribe.whisper-base.v1";
    return false;
  }
  if (!valid_utf8(spec.input_uris(0)) || has_embedded_nul(spec.input_uris(0))) {
    error = "input path must be well-formed UTF-8 without NUL bytes";
    return false;
  }
  parsed.input = utf8_path(spec.input_uris(0));
  if (!parsed.input.is_absolute()) {
    error = "input path must be absolute";
    return false;
  }
  if (!parsed.options.ParseFromString(spec.options())) {
    error = "options is not a valid TranscribeOptions protobuf";
    return false;
  }
  if (parsed.options.GetReflection()->GetUnknownFields(parsed.options).field_count() != 0) {
    error = "options contains unknown fields";
    return false;
  }
  if (!transcription::validate_options(parsed.options, error)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool run_transcribe(const protocol::JobSpec& spec, const EventSink& sink,
                                  DispatchDependencies& dependencies,
                                  const jobs::CancellationRegistry::Token& cancel_token) {
  ParsedTranscribeRequest parsed;
  std::string validation_error;
  if (!parse_transcribe_request(spec, parsed, validation_error)) {
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "validating");
    fail(event, "invalid-argument", 0, "The transcription settings are not valid.",
         validation_error);
    return sink(event);
  }
  if (dependencies.transcriber == nullptr) {
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "backend-unavailable");
    fail(event, "backend-unavailable", 0, "Transcription is unavailable in this worker build.",
         "whisper.cpp was not configured or no transcription service was injected");
    return sink(event);
  }
  if (!dependencies.transcriber->capabilities().available) {
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "backend-unavailable");
    fail(event, "backend-unavailable", 0, "Transcription is unavailable in this worker build.",
         "whisper.cpp backend capability is not available");
    return sink(event);
  }

  std::stop_source stop;
  RegistryCancelWatcher cancel_watcher(cancel_token, stop);
  double last_progress = 0.0;
  bool sink_available = true;
  const auto progress = [&](const double value, const std::string_view phase) {
    if (!sink_available)
      return;
    if (cancel_token->load(std::memory_order_acquire)) {
      stop.request_stop();
    }
    const double next = std::max(last_progress, std::clamp(value, 0.0, 1.0));
    auto event = event_for(spec, protocol::JOB_STATE_RUNNING, next, phase);
    sink_available = sink(event);
    last_progress = next;
    if (!sink_available)
      stop.request_stop();
  };
  const auto result = dependencies.transcriber->transcribe(parsed.input, parsed.options,
                                                           stop.get_token(), progress);
  if (!sink_available)
    return false;
  if (!result) {
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, last_progress, "failed");
    const auto& issue = result.error();
    const auto category = [&]() -> std::string_view {
      using transcription::ErrorCode;
      switch (issue.code) {
      case ErrorCode::Cancelled:
        return "cancelled";
      case ErrorCode::BackendUnavailable:
        return "backend-unavailable";
      case ErrorCode::ModelDownloadFailed:
        return "model-download";
      case ErrorCode::ModelChecksumMismatch:
        return "model-checksum";
      case ErrorCode::ModelSizeMismatch:
        return "model-size";
      case ErrorCode::InputNotFound:
        return "source-not-found";
      case ErrorCode::AudioDecodeFailed:
        return "audio-decode";
      case ErrorCode::InvalidOptions:
      case ErrorCode::InvalidInput:
        return "invalid-argument";
      case ErrorCode::ModelUnavailable:
      case ErrorCode::BackendFailed:
        return "transcription";
      }
      return "transcription";
    }();
    fail(event, category, issue.native_code, "Transcription failed.", issue.message,
         issue.retryable);
    if (issue.code == transcription::ErrorCode::Cancelled) {
      event.mutable_event()->set_state(protocol::JOB_STATE_CANCELLED);
      event.mutable_event()->set_phase("cancelled");
    }
    return sink(event);
  }
  auto event = event_for(spec, protocol::JOB_STATE_SUCCEEDED, 1.0, "complete");
  if (!result.value().SerializeToString(event.mutable_event()->mutable_result())) {
    fail(event, "serialization", 0, "Transcription completed but its result could not be saved.",
         "TranscriptionResult protobuf serialization failed");
    return sink(event);
  }
  auto* metadata = event.mutable_event()->mutable_metadata();
  (*metadata)["contract_version"] = std::to_string(transcription::kTranscriptionSchemaVersion);
  (*metadata)["backend"] = result.value().backend();
  (*metadata)["model_id"] = result.value().model_id();
  (*metadata)["detected_language"] = result.value().detected_language();
  (*metadata)["word_count"] = std::to_string(result.value().words_size());
  (*metadata)["vulkan_capability"] =
      result.value().vulkan_available() ? "compiled/asserted" : "not-asserted";
  (*metadata)["vulkan_used"] = "unknown";
  return sink(event);
}

[[nodiscard]] std::optional<export_service::PlatformPreset>
platform_preset_from_int(const int value) {
  if (value < static_cast<int>(export_service::PlatformPreset::ReferenceFfv1) ||
      value > static_cast<int>(export_service::PlatformPreset::PodcastAudioOnly)) {
    return std::nullopt;
  }
  return static_cast<export_service::PlatformPreset>(value);
}

[[nodiscard]] std::optional<export_service::CaptionExportMode>
caption_mode_from_string(const std::string_view value) {
  if (value.empty() || value == "none") {
    return export_service::CaptionExportMode::None;
  }
  if (value == "burn_in") {
    return export_service::CaptionExportMode::BurnIn;
  }
  if (value == "sidecar") {
    return export_service::CaptionExportMode::Sidecar;
  }
  if (value == "burn_in_and_sidecar") {
    return export_service::CaptionExportMode::BurnInAndSidecar;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<export_service::SidecarFormat>
sidecar_format_from_string(const std::string_view value) {
  if (value.empty() || value == "srt") {
    return export_service::SidecarFormat::Srt;
  }
  if (value == "vtt") {
    return export_service::SidecarFormat::WebVtt;
  }
  return std::nullopt;
}

struct ParsedExportRequest final {
  std::filesystem::path checkpoint;
  std::filesystem::path destination;
  protocol::ExportOptions options;
  export_service::PlatformPreset platform{};
  export_service::CaptionExportMode caption_mode{export_service::CaptionExportMode::None};
  export_service::SidecarFormat sidecar_format{export_service::SidecarFormat::Srt};
  edit::EntityId sequence_id{};
};

[[nodiscard]] bool parse_export_request(const protocol::JobSpec& spec, ParsedExportRequest& parsed,
                                        std::string& error) {
  if (!jobs::valid_job_id(spec.job_id())) {
    error = "job_id must be a canonical UUID job identifier";
    return false;
  }
  if (spec.input_uris_size() != 0) {
    error = "input_uris must be empty for export; media paths come from the project checkpoint";
    return false;
  }
  if (spec.project_checkpoint().empty() || spec.output_uri().empty()) {
    error = "project_checkpoint and output_uri must not be empty";
    return false;
  }
  if (has_embedded_nul(spec.project_checkpoint()) || has_embedded_nul(spec.output_uri())) {
    error = "checkpoint and output paths must not contain NUL bytes";
    return false;
  }
  if (!valid_utf8(spec.project_checkpoint()) || !valid_utf8(spec.output_uri())) {
    error = "checkpoint and output paths must be well-formed UTF-8";
    return false;
  }
  if (spec.preset_id() != kExportPreset) {
    error = "preset_id must be video-editor.export.creator.v1";
    return false;
  }
  parsed.checkpoint = utf8_path(spec.project_checkpoint());
  parsed.destination = utf8_path(spec.output_uri());
  if (!parsed.checkpoint.is_absolute() || !parsed.destination.is_absolute()) {
    error = "checkpoint and output must be absolute local filesystem paths";
    return false;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(parsed.checkpoint, filesystem_error)) {
    error = filesystem_error
                ? "checkpoint path could not be inspected: " + filesystem_error.message()
                : "project_checkpoint is not a regular file";
    return false;
  }
  filesystem_error.clear();
  const bool destination_exists = std::filesystem::exists(parsed.destination, filesystem_error);
  if (filesystem_error) {
    error = "output path could not be inspected: " + filesystem_error.message();
    return false;
  }
  if (destination_exists && std::filesystem::is_directory(parsed.destination, filesystem_error)) {
    error = "output path refers to a directory";
    return false;
  }
  if (filesystem_error) {
    error = "output path could not be inspected: " + filesystem_error.message();
    return false;
  }
  if (!parsed.options.ParseFromString(spec.options())) {
    error = "options is not a valid ExportOptions protobuf";
    return false;
  }
  if (parsed.options.GetReflection()->GetUnknownFields(parsed.options).field_count() != 0) {
    error = "options contains unknown fields";
    return false;
  }
  if (parsed.options.schema_version() != kExportSchemaVersion) {
    error = "schema_version must be 1";
    return false;
  }
  const auto platform = platform_preset_from_int(parsed.options.platform_preset());
  if (!platform.has_value()) {
    error = "platform_preset is not a recognized export preset";
    return false;
  }
  const auto caption_mode = caption_mode_from_string(parsed.options.caption_mode());
  if (!caption_mode.has_value()) {
    error = "caption_mode must be none, burn_in, sidecar, or burn_in_and_sidecar";
    return false;
  }
  const auto sidecar_format = sidecar_format_from_string(parsed.options.sidecar_format());
  if (!sidecar_format.has_value()) {
    error = "sidecar_format must be srt or vtt";
    return false;
  }
  if (has_embedded_nul(parsed.options.sequence_id()) || !valid_utf8(parsed.options.sequence_id())) {
    error = "sequence_id must be well-formed UTF-8 without NUL bytes";
    return false;
  }
  const auto sequence_id = edit::EntityId::parse(parsed.options.sequence_id());
  if (!sequence_id.has_value() || sequence_id->isNil()) {
    error = "sequence_id must be a canonical entity identifier";
    return false;
  }
  parsed.platform = *platform;
  parsed.caption_mode = *caption_mode;
  parsed.sidecar_format = *sidecar_format;
  parsed.sequence_id = *sequence_id;
  return true;
}

[[nodiscard]] bool emit_export_invalid(const protocol::JobSpec& spec, const EventSink& sink,
                                       const std::string_view diagnostic) {
  auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "validating");
  fail(event, "invalid-argument", 0, "The export job settings are not valid.", diagnostic);
  return sink(event);
}

struct ExportErrorDescription {
  std::string_view category;
  std::string_view user_message;
  bool retryable{false};
};

[[nodiscard]] ExportErrorDescription describe(const export_service::ExportErrorCode code) {
  using export_service::ExportErrorCode;
  switch (code) {
  case ExportErrorCode::InvalidRequest:
    return {"invalid-argument", "The export settings are not valid."};
  case ExportErrorCode::AudioRendererRequired:
    return {"invalid-argument", "This export needs an audio renderer."};
  case ExportErrorCode::AudioRenderFailed:
    return {"audio-render", "The timeline audio could not be rendered."};
  case ExportErrorCode::DestinationExists:
    return {"destination-exists", "The export destination already exists."};
  case ExportErrorCode::EncoderUnavailable:
    return {"encoder-unavailable", "The selected export encoder is not available."};
  case ExportErrorCode::HardwareEncoderFailed:
    return {"hardware-encoder", "Hardware encoding failed."};
  case ExportErrorCode::Cancelled:
    return {"cancelled", "Export was cancelled."};
  case ExportErrorCode::RenderFailed:
    return {"render", "A timeline frame could not be rendered."};
  case ExportErrorCode::EncodingFailed:
    return {"media-encode", "The export could not be encoded."};
  case ExportErrorCode::IoFailed:
    return {"io-write", "The export could not be written.", true};
  case ExportErrorCode::CommitFailed:
    return {"io-commit", "The export could not be saved.", true};
  case ExportErrorCode::ProgressCallbackFailed:
    return {"internal", "Export progress reporting failed."};
  }
  return {"internal", "Export failed unexpectedly."};
}

[[nodiscard]] bool load_checkpoint_bytes(const std::filesystem::path& path,
                                         std::vector<std::byte>& bytes, std::string& error) {
  std::error_code filesystem_error;
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error) {
    error = "checkpoint could not be read: " + filesystem_error.message();
    return false;
  }
  if (size == 0 || size > project_codec::kMaximumSnapshotBytes) {
    error = "checkpoint size is outside the supported snapshot range";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "checkpoint could not be opened";
    return false;
  }
  bytes.resize(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
    error = "checkpoint could not be read completely";
    return false;
  }
  return true;
}

[[nodiscard]] bool register_export_media(const edit::Project& project,
                                         playback::AssetRegistry& playback_registry,
                                         audio_render::OriginalAudioRegistry& audio_registry,
                                         std::string& error) {
  for (const edit::Asset& asset : project.assets) {
    if (asset.source_uri.empty()) {
      continue;
    }
    if (has_embedded_nul(asset.source_uri) || !valid_utf8(asset.source_uri)) {
      error = "asset source_uri must be well-formed UTF-8 without NUL bytes";
      return false;
    }
    const std::filesystem::path source = utf8_path(asset.source_uri);
    if (!source.is_absolute()) {
      error = "asset source_uri must be an absolute local filesystem path";
      return false;
    }
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(source, filesystem_error)) {
      error = filesystem_error ? "asset source could not be inspected: " + filesystem_error.message()
                               : "asset source_uri is not a regular file";
      return false;
    }
    if (asset.has_video || !asset.has_audio) {
      if (!playback_registry.register_asset(
              asset.id, playback::AssetPlaybackSources{
                            .original = {.path = source, .video_stream_index = -1},
                            .proxy = std::nullopt})) {
        error = "could not register an original playback source";
        return false;
      }
    }
    if (asset.has_audio &&
        !audio_registry.register_original(
            asset.id, audio_render::OriginalAudioMedia{.path = source, .audio_stream_index = -1})) {
      error = "could not register an original audio source";
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool run_export(const protocol::JobSpec& spec, const EventSink& sink,
                              const jobs::CancellationRegistry::Token& cancel_token) {
  ParsedExportRequest parsed;
  std::string validation_error;
  if (!parse_export_request(spec, parsed, validation_error)) {
    return emit_export_invalid(spec, sink, validation_error);
  }

  std::vector<std::byte> checkpoint_bytes;
  if (!load_checkpoint_bytes(parsed.checkpoint, checkpoint_bytes, validation_error)) {
    return emit_export_invalid(spec, sink, validation_error);
  }
  auto decoded = project_codec::deserialize_project(std::span<const std::byte>(checkpoint_bytes));
  if (!decoded) {
    return emit_export_invalid(spec, sink, decoded.error().message);
  }

  edit::TimelineEditor editor(std::move(decoded).value());
  auto snapshot_result = editor.snapshot(parsed.sequence_id, editor.revision());
  if (!snapshot_result) {
    return emit_export_invalid(spec, sink, snapshot_result.error().message);
  }
  auto snapshot = std::move(snapshot_result).value();

  auto playback_registry = std::make_shared<playback::AssetRegistry>();
  auto audio_registry = std::make_shared<audio_render::OriginalAudioRegistry>();
  if (!register_export_media(snapshot.project(), *playback_registry, *audio_registry,
                             validation_error)) {
    return emit_export_invalid(spec, sink, validation_error);
  }

  const auto video_preset = export_service::reference_video_preset_for(parsed.platform);
  if (!video_preset.has_value()) {
    return emit_export_invalid(spec, sink, "platform_preset has no FOSS export mapping");
  }

  class EpochSyncFrameProvider final : public render::FrameProvider {
  public:
    explicit EpochSyncFrameProvider(std::shared_ptr<playback::FfmpegFrameProvider> provider)
        : provider_(std::move(provider)) {}

    render::RenderResult<std::shared_ptr<const render::CpuFrame>>
    request(const render::AssetFrameRequest& request) override {
      provider_->begin_epoch(request.request_epoch);
      return provider_->request(request);
    }

  private:
    std::shared_ptr<playback::FfmpegFrameProvider> provider_;
  };

  auto frame_provider = std::make_shared<playback::FfmpegFrameProvider>(playback_registry);
  auto synchronized_provider = std::make_shared<EpochSyncFrameProvider>(frame_provider);
  auto renderer = std::make_shared<render::CpuRenderer>(synchronized_provider);
  auto audio_renderer = std::make_shared<audio_render::TimelineAudioRenderer>(audio_registry);
  const auto captions = snapshot.sequence().captions;

  std::stop_source internal_stop;
  RegistryCancelWatcher cancel_watcher(cancel_token, internal_stop);
  double last_progress = 0.0;
  bool sink_available = true;
  const auto progress = [&](const export_service::ExportProgress& update) {
    if (!sink_available) {
      return;
    }
    if (cancel_token->load(std::memory_order_acquire)) {
      internal_stop.request_stop();
    }
    if (update.restarted_after_hardware_fallback) {
      last_progress = 0.0;
      auto event = event_for(spec, protocol::JOB_STATE_RUNNING, 0.0, "hardware-fallback");
      auto* metadata = event.mutable_event()->mutable_metadata();
      (*metadata)["restarted_after_hardware_fallback"] = "true";
      sink_available = sink(event);
      if (!sink_available) {
        internal_stop.request_stop();
      }
      return;
    }
    const double next = std::max(last_progress, std::clamp(update.fraction, 0.0, 1.0));
    auto event = event_for(spec, protocol::JOB_STATE_RUNNING, next, "exporting");
    sink_available = sink(event);
    last_progress = next;
    if (!sink_available) {
      internal_stop.request_stop();
    }
  };

  export_service::ExportRequest request{
      .snapshot = std::move(snapshot),
      .renderer = std::move(renderer),
      .audio_renderer = std::move(audio_renderer),
      .destination = parsed.destination,
      .preset = *video_preset,
      .overwrite_existing = parsed.options.overwrite_existing(),
      .include_audio = parsed.options.include_audio(),
      .prefer_hardware_encoder = parsed.options.prefer_hardware(),
      .cancellation = internal_stop.get_token(),
      .progress = progress,
      .platform_preset = parsed.platform,
      .caption_mode = parsed.caption_mode,
      .sidecar_format = parsed.sidecar_format,
      .override_width = parsed.options.override_width(),
      .override_height = parsed.options.override_height(),
      .override_frame_rate_num = parsed.options.override_frame_rate_num(),
      .override_frame_rate_den = parsed.options.override_frame_rate_den(),
      .override_audio_bitrate = parsed.options.override_audio_bitrate(),
      .override_video_bitrate = parsed.options.override_video_bitrate(),
      .video_quality = parsed.options.has_video_quality()
                           ? std::optional<int>{parsed.options.video_quality()}
                           : std::nullopt,
      .captions = captions};

  const auto exported = export_service::export_video(request);
  if (!sink_available) {
    return false;
  }
  if (!exported) {
    const ExportErrorDescription description = describe(exported.error().code);
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, last_progress, "failed");
    fail(event, description.category, 0, description.user_message, exported.error().message,
         description.retryable);
    if (exported.error().code == export_service::ExportErrorCode::Cancelled) {
      event.mutable_event()->set_state(protocol::JOB_STATE_CANCELLED);
      event.mutable_event()->set_phase("cancelled");
    }
    return sink(event);
  }

  const export_service::ExportResult& result = exported.value();
  auto event = event_for(spec, protocol::JOB_STATE_SUCCEEDED, 1.0, "complete");
  protocol::JobEvent* job_event = event.mutable_event();
  job_event->set_result_uri(utf8_string(result.destination));
  auto* metadata = job_event->mutable_metadata();
  (*metadata)["contract_version"] = "1";
  (*metadata)["preset_id"] = std::string(kExportPreset);
  (*metadata)["frame_count"] = std::to_string(result.frame_count);
  (*metadata)["audio_sample_count"] = std::to_string(result.audio_sample_count);
  (*metadata)["video_encoder"] = result.video_encoder;
  (*metadata)["hardware_encoder_used"] = result.hardware_encoder_used ? "true" : "false";
  return sink(event);
}

[[nodiscard]] bool write_bytes_atomically(const std::filesystem::path& destination,
                                          const std::span<const std::byte> bytes,
                                          std::string& error) {
  if (destination.empty() || bytes.empty()) {
    error = "destination and bytes must not be empty";
    return false;
  }
  const std::filesystem::path parent = destination.parent_path();
  std::error_code filesystem_error;
  if (!parent.empty() && !std::filesystem::exists(parent, filesystem_error)) {
    if (!std::filesystem::create_directories(parent, filesystem_error) || filesystem_error) {
      error = filesystem_error ? "could not create output parent directory: " +
                                     filesystem_error.message()
                               : "could not create output parent directory";
      return false;
    }
  }
  const std::filesystem::path temp_path =
      parent / (destination.filename().string() + ".veworker.tmp");
  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "could not open temporary output file";
      return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      std::error_code ignored;
      std::filesystem::remove(temp_path, ignored);
      error = "could not write temporary output file";
      return false;
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(temp_path, destination, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    error = "could not install output file: " + rename_error.message();
    return false;
  }
  return true;
}

struct ParsedThumbnailRequest final {
  std::filesystem::path input;
  std::filesystem::path output;
  media_cache::ThumbnailOptions options;
  std::string asset_id;
  int stream_index{-1};
};

[[nodiscard]] bool thumbnail_strategy_from_int(const int value,
                                               media_cache::ThumbnailOptions::Strategy& strategy,
                                               std::string& error) {
  switch (value) {
  case 0:
    strategy = media_cache::ThumbnailOptions::Strategy::First;
    return true;
  case 1:
    strategy = media_cache::ThumbnailOptions::Strategy::Middle;
    return true;
  case 2:
    strategy = media_cache::ThumbnailOptions::Strategy::Last;
    return true;
  default:
    error = "strategy must be 0 (First), 1 (Middle), or 2 (Last)";
    return false;
  }
}

[[nodiscard]] bool parse_thumbnail_request(const protocol::JobSpec& spec,
                                         ParsedThumbnailRequest& parsed, std::string& error) {
  if (!jobs::valid_job_id(spec.job_id())) {
    error = "job_id must be a canonical UUID job identifier";
    return false;
  }
  if (spec.input_uris_size() != 1 || spec.input_uris(0).empty() || spec.output_uri().empty()) {
    error = "input_uris must contain exactly one source path and output_uri must not be empty";
    return false;
  }
  if (!spec.project_checkpoint().empty()) {
    error = "project_checkpoint must be empty for thumbnail jobs";
    return false;
  }
  if (spec.preset_id() != kThumbnailPreset) {
    error = "preset_id must be video-editor.thumbnail.v1";
    return false;
  }
  if (has_embedded_nul(spec.input_uris(0)) || has_embedded_nul(spec.output_uri()) ||
      !valid_utf8(spec.input_uris(0)) || !valid_utf8(spec.output_uri())) {
    error = "source and output paths must be well-formed UTF-8 without NUL bytes";
    return false;
  }
  parsed.input = utf8_path(spec.input_uris(0));
  parsed.output = utf8_path(spec.output_uri());
  if (!parsed.input.is_absolute() || !parsed.output.is_absolute()) {
    error = "source and output must be absolute local filesystem paths";
    return false;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(parsed.input, filesystem_error)) {
    error = filesystem_error ? "source path could not be inspected: " + filesystem_error.message()
                             : "source path is not a regular file";
    return false;
  }
  filesystem_error.clear();
  if (std::filesystem::exists(parsed.output, filesystem_error) &&
      std::filesystem::is_directory(parsed.output, filesystem_error)) {
    error = "output path refers to a directory";
    return false;
  }
  if (filesystem_error) {
    error = "output path could not be inspected: " + filesystem_error.message();
    return false;
  }

  protocol::ThumbnailJobOptions options;
  if (!options.ParseFromString(spec.options())) {
    error = "options is not a valid ThumbnailJobOptions protobuf";
    return false;
  }
  if (options.GetReflection()->GetUnknownFields(options).field_count() != 0) {
    error = "options contains unknown fields";
    return false;
  }
  if (options.schema_version() != kThumbnailJobSchemaVersion) {
    error = "schema_version must be 1";
    return false;
  }
  if (options.asset_id().empty() || has_embedded_nul(options.asset_id()) ||
      !valid_utf8(options.asset_id())) {
    error = "asset_id must be well-formed UTF-8 without NUL bytes";
    return false;
  }
  parsed.asset_id = options.asset_id();
  parsed.stream_index = options.stream_index();
  parsed.options.maximum_width = options.maximum_width() == 0 ? 320 : options.maximum_width();
  parsed.options.quality = options.quality() == 0 ? 85 : options.quality();
  if (!thumbnail_strategy_from_int(options.strategy(), parsed.options.strategy, error)) {
    return false;
  }
  return true;
}

struct ThumbnailErrorDescription {
  std::string_view category;
  std::string_view user_message;
  bool retryable{false};
};

[[nodiscard]] ThumbnailErrorDescription describe(const media_cache::ThumbnailErrorCode code) {
  using media_cache::ThumbnailErrorCode;
  switch (code) {
  case ThumbnailErrorCode::InvalidArgument:
    return {"invalid-argument", "The thumbnail settings are not valid."};
  case ThumbnailErrorCode::SourceNotFound:
  case ThumbnailErrorCode::NotFound:
    return {"source-not-found", "The source media is missing or unreadable."};
  case ThumbnailErrorCode::NoVideoStream:
    return {"no-video-stream", "The source media has no video stream."};
  case ThumbnailErrorCode::OpenFailed:
    return {"media-open", "The source media could not be opened."};
  case ThumbnailErrorCode::DecodeFailed:
    return {"media-decode", "The source media could not be decoded."};
  case ThumbnailErrorCode::ScaleFailed:
    return {"media-scale", "The thumbnail could not be scaled."};
  case ThumbnailErrorCode::EncodeFailed:
    return {"media-encode", "The thumbnail could not be encoded."};
  case ThumbnailErrorCode::StoreFailed:
    return {"io-write", "The thumbnail could not be saved.", true};
  case ThumbnailErrorCode::Cancelled:
    return {"cancelled", "Thumbnail generation was cancelled."};
  case ThumbnailErrorCode::Internal:
  case ThumbnailErrorCode::None:
    return {"internal", "Thumbnail generation failed unexpectedly."};
  }
  return {"internal", "Thumbnail generation failed unexpectedly."};
}

[[nodiscard]] bool run_thumbnail(const protocol::JobSpec& spec, const EventSink& sink,
                                 const jobs::CancellationRegistry::Token& cancel_token) {
  ParsedThumbnailRequest parsed;
  std::string validation_error;
  if (!parse_thumbnail_request(spec, parsed, validation_error)) {
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "validating");
    fail(event, "invalid-argument", 0, "The thumbnail settings are not valid.", validation_error);
    return sink(event);
  }

  const std::filesystem::path staging_root =
      parsed.output.parent_path() / (".veworker-thumb-" + spec.job_id());
  std::error_code cleanup_error;
  std::filesystem::remove_all(staging_root, cleanup_error);

  std::stop_source internal_stop;
  RegistryCancelWatcher cancel_watcher(cancel_token, internal_stop);
  if (!sink(event_for(spec, protocol::JOB_STATE_RUNNING, 0.1, "decoding"))) {
    return false;
  }
  media_cache::CacheStore staging_store(staging_root,
                                        media_cache::CacheStoreOptions{.integrity_check = false});
  const auto generated = media_cache::generate_thumbnail(
      parsed.input, parsed.stream_index, parsed.options, parsed.asset_id, staging_store,
      internal_stop.get_token());
  std::filesystem::remove_all(staging_root, cleanup_error);
  if (!generated) {
    std::error_code ignored;
    std::filesystem::remove(parsed.output, ignored);
    const ThumbnailErrorDescription description = describe(generated.error().code);
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "failed");
    fail(event, description.category, generated.error().native_code, description.user_message,
         generated.error().message, description.retryable);
    if (generated.error().code == media_cache::ThumbnailErrorCode::Cancelled) {
      event.mutable_event()->set_state(protocol::JOB_STATE_CANCELLED);
      event.mutable_event()->set_phase("cancelled");
    }
    return sink(event);
  }

  std::string write_error;
  if (!write_bytes_atomically(parsed.output,
                              std::span<const std::byte>(generated.value().jpeg_bytes.data(),
                                                         generated.value().jpeg_bytes.size()),
                              write_error)) {
    std::error_code ignored;
    std::filesystem::remove(parsed.output, ignored);
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "failed");
    fail(event, "io-write", 0, "The thumbnail could not be saved.", write_error, true);
    return sink(event);
  }

  const media_cache::Thumbnail& thumbnail = generated.value();
  auto event = event_for(spec, protocol::JOB_STATE_SUCCEEDED, 1.0, "complete");
  protocol::JobEvent* job_event = event.mutable_event();
  job_event->set_result_uri(utf8_string(parsed.output));
  auto* metadata = job_event->mutable_metadata();
  (*metadata)["contract_version"] = "1";
  (*metadata)["preset_id"] = std::string(kThumbnailPreset);
  (*metadata)["asset_id"] = parsed.asset_id;
  (*metadata)["parameter_hash"] = media_cache::thumbnail_parameter_hash(parsed.options);
  (*metadata)["width"] = std::to_string(thumbnail.width);
  (*metadata)["height"] = std::to_string(thumbnail.height);
  (*metadata)["source_pts_us"] = std::to_string(thumbnail.source_pts_microseconds);
  return sink(event);
}

struct ParsedWaveformRequest final {
  std::filesystem::path input;
  std::filesystem::path output;
  media_cache::WaveformOptions options;
  std::string asset_id;
  int stream_index{-1};
};

[[nodiscard]] bool parse_waveform_request(const protocol::JobSpec& spec, ParsedWaveformRequest& parsed,
                                          std::string& error) {
  if (!jobs::valid_job_id(spec.job_id())) {
    error = "job_id must be a canonical UUID job identifier";
    return false;
  }
  if (spec.input_uris_size() != 1 || spec.input_uris(0).empty() || spec.output_uri().empty()) {
    error = "input_uris must contain exactly one source path and output_uri must not be empty";
    return false;
  }
  if (!spec.project_checkpoint().empty()) {
    error = "project_checkpoint must be empty for waveform jobs";
    return false;
  }
  if (spec.preset_id() != kWaveformPreset) {
    error = "preset_id must be video-editor.waveform.v1";
    return false;
  }
  if (has_embedded_nul(spec.input_uris(0)) || has_embedded_nul(spec.output_uri()) ||
      !valid_utf8(spec.input_uris(0)) || !valid_utf8(spec.output_uri())) {
    error = "source and output paths must be well-formed UTF-8 without NUL bytes";
    return false;
  }
  parsed.input = utf8_path(spec.input_uris(0));
  parsed.output = utf8_path(spec.output_uri());
  if (!parsed.input.is_absolute() || !parsed.output.is_absolute()) {
    error = "source and output must be absolute local filesystem paths";
    return false;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(parsed.input, filesystem_error)) {
    error = filesystem_error ? "source path could not be inspected: " + filesystem_error.message()
                             : "source path is not a regular file";
    return false;
  }
  filesystem_error.clear();
  if (std::filesystem::exists(parsed.output, filesystem_error) &&
      std::filesystem::is_directory(parsed.output, filesystem_error)) {
    error = "output path refers to a directory";
    return false;
  }
  if (filesystem_error) {
    error = "output path could not be inspected: " + filesystem_error.message();
    return false;
  }

  protocol::WaveformJobOptions options;
  if (!options.ParseFromString(spec.options())) {
    error = "options is not a valid WaveformJobOptions protobuf";
    return false;
  }
  if (options.GetReflection()->GetUnknownFields(options).field_count() != 0) {
    error = "options contains unknown fields";
    return false;
  }
  if (options.schema_version() != kWaveformJobSchemaVersion) {
    error = "schema_version must be 1";
    return false;
  }
  if (options.asset_id().empty() || has_embedded_nul(options.asset_id()) ||
      !valid_utf8(options.asset_id())) {
    error = "asset_id must be well-formed UTF-8 without NUL bytes";
    return false;
  }
  parsed.asset_id = options.asset_id();
  parsed.stream_index = options.stream_index();
  parsed.options.finest_level_buckets =
      options.finest_level_buckets() == 0 ? 2000 : options.finest_level_buckets();
  parsed.options.level_count = options.level_count() == 0 ? 8 : options.level_count();
  parsed.options.sample_rate = options.sample_rate() == 0 ? 48'000 : options.sample_rate();
  parsed.options.channel_count = options.channel_count() == 0 ? 1 : options.channel_count();
  return true;
}

struct WaveformErrorDescription {
  std::string_view category;
  std::string_view user_message;
  bool retryable{false};
};

[[nodiscard]] WaveformErrorDescription describe(const media_cache::WaveformErrorCode code) {
  using media_cache::WaveformErrorCode;
  switch (code) {
  case WaveformErrorCode::InvalidArgument:
    return {"invalid-argument", "The waveform settings are not valid."};
  case WaveformErrorCode::SourceNotFound:
  case WaveformErrorCode::NotFound:
    return {"source-not-found", "The source media is missing or unreadable."};
  case WaveformErrorCode::NoAudioStream:
    return {"no-audio-stream", "The source media has no audio stream."};
  case WaveformErrorCode::OpenFailed:
    return {"media-open", "The source media could not be opened."};
  case WaveformErrorCode::DecodeFailed:
    return {"media-decode", "The source media could not be decoded."};
  case WaveformErrorCode::ResampleFailed:
    return {"media-resample", "The waveform could not be resampled."};
  case WaveformErrorCode::StoreFailed:
    return {"io-write", "The waveform could not be saved.", true};
  case WaveformErrorCode::Cancelled:
    return {"cancelled", "Waveform generation was cancelled."};
  case WaveformErrorCode::Internal:
  case WaveformErrorCode::None:
    return {"internal", "Waveform generation failed unexpectedly."};
  }
  return {"internal", "Waveform generation failed unexpectedly."};
}

[[nodiscard]] bool run_waveform(const protocol::JobSpec& spec, const EventSink& sink,
                                const jobs::CancellationRegistry::Token& cancel_token) {
  ParsedWaveformRequest parsed;
  std::string validation_error;
  if (!parse_waveform_request(spec, parsed, validation_error)) {
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "validating");
    fail(event, "invalid-argument", 0, "The waveform settings are not valid.", validation_error);
    return sink(event);
  }

  const std::filesystem::path staging_root =
      parsed.output.parent_path() / (".veworker-wave-" + spec.job_id());
  std::error_code cleanup_error;
  std::filesystem::remove_all(staging_root, cleanup_error);

  std::stop_source internal_stop;
  RegistryCancelWatcher cancel_watcher(cancel_token, internal_stop);
  if (!sink(event_for(spec, protocol::JOB_STATE_RUNNING, 0.1, "decoding"))) {
    return false;
  }
  media_cache::CacheStore staging_store(staging_root,
                                        media_cache::CacheStoreOptions{.integrity_check = false});
  const auto generated = media_cache::generate_waveform(
      parsed.input, parsed.stream_index, parsed.options, parsed.asset_id, staging_store,
      internal_stop.get_token());
  std::filesystem::remove_all(staging_root, cleanup_error);
  if (!generated) {
    std::error_code ignored;
    std::filesystem::remove(parsed.output, ignored);
    const WaveformErrorDescription description = describe(generated.error().code);
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "failed");
    fail(event, description.category, generated.error().native_code, description.user_message,
         generated.error().message, description.retryable);
    if (generated.error().code == media_cache::WaveformErrorCode::Cancelled) {
      event.mutable_event()->set_state(protocol::JOB_STATE_CANCELLED);
      event.mutable_event()->set_phase("cancelled");
    }
    return sink(event);
  }

  const std::vector<std::byte> blob = media_cache::serialize_waveform(generated.value());
  std::string write_error;
  if (!write_bytes_atomically(parsed.output, blob, write_error)) {
    std::error_code ignored;
    std::filesystem::remove(parsed.output, ignored);
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "failed");
    fail(event, "io-write", 0, "The waveform could not be saved.", write_error, true);
    return sink(event);
  }

  auto event = event_for(spec, protocol::JOB_STATE_SUCCEEDED, 1.0, "complete");
  protocol::JobEvent* job_event = event.mutable_event();
  job_event->set_result_uri(utf8_string(parsed.output));
  auto* metadata = job_event->mutable_metadata();
  (*metadata)["contract_version"] = "1";
  (*metadata)["preset_id"] = std::string(kWaveformPreset);
  (*metadata)["asset_id"] = parsed.asset_id;
  (*metadata)["parameter_hash"] = media_cache::waveform_parameter_hash(parsed.options);
  (*metadata)["level_count"] = std::to_string(generated.value().levels.size());
  (*metadata)["total_samples"] = std::to_string(generated.value().total_samples);
  return sink(event);
}

} // namespace

bool dispatch_job(const protocol::JobSpec& spec, const EventSink& sink) {
  DispatchDependencies dependencies;
  jobs::CancellationRegistry registry;
  return dispatch_job(spec, sink, dependencies, registry);
}

bool dispatch_job(const protocol::JobSpec& spec, const EventSink& sink,
                  DispatchDependencies& dependencies) {
  jobs::CancellationRegistry registry;
  return dispatch_job(spec, sink, dependencies, registry);
}

bool dispatch_job(const protocol::JobSpec& spec, const EventSink& sink,
                  DispatchDependencies& dependencies, jobs::CancellationRegistry& registry) {
  if (!sink) {
    return false;
  }
  const jobs::CancellationRegistry::Token cancel_token = registry.begin(spec.job_id());
  struct FinishGuard final {
    jobs::CancellationRegistry& registry;
    std::string job_id;
    ~FinishGuard() {
      registry.finish(job_id);
    }
  } finish_guard{registry, spec.job_id()};
  if (!sink(event_for(spec, protocol::JOB_STATE_ACCEPTED, 0.0, "accepted"))) {
    return false;
  }
  if (spec.kind() == protocol::JOB_KIND_PROBE) {
    return run_probe(spec, sink);
  }
  if (spec.kind() == protocol::JOB_KIND_PROXY) {
    return run_proxy(spec, sink, cancel_token);
  }
  if (spec.kind() == protocol::JOB_KIND_TRANSCRIBE) {
    return run_transcribe(spec, sink, dependencies, cancel_token);
  }
  if (spec.kind() == protocol::JOB_KIND_EXPORT) {
    return run_export(spec, sink, cancel_token);
  }
  if (spec.kind() == protocol::JOB_KIND_THUMBNAIL) {
    return run_thumbnail(spec, sink, cancel_token);
  }
  if (spec.kind() == protocol::JOB_KIND_WAVEFORM) {
    return run_waveform(spec, sink, cancel_token);
  }
  auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "unsupported");
  fail(event, "unsupported-job", 0, "This worker does not implement that job yet.",
       "only JOB_KIND_PROBE, JOB_KIND_PROXY, JOB_KIND_TRANSCRIBE, JOB_KIND_EXPORT, "
       "JOB_KIND_THUMBNAIL, and JOB_KIND_WAVEFORM are enabled in this worker");
  return sink(event);
}

bool handle_cancel_job(const protocol::CancelJob& request, jobs::CancellationRegistry& registry,
                       const EventSink& sink) {
  if (registry.cancel(request.job_id())) {
    return true;
  }
  if (!sink) {
    return false;
  }
  protocol::JobSpec spec;
  spec.set_job_id(request.job_id());
  auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "cancellation-rejected");
  fail(event, "job-not-found", 0, "No running job matches that identifier.",
       "CancelJob referenced a job that is not active in this worker");
  return sink(event);
}

} // namespace video_editor::workers
