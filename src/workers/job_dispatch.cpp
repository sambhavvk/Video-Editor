// SPDX-License-Identifier: MPL-2.0
#include "video_editor/workers/job_dispatch.h"

#include "video_editor/job_service/job_id.h"
#include "video_editor/media_codec/probe.h"
#include "video_editor/proxy_service/proxy_service.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace video_editor::workers {
namespace {

using jobs::kProtocolMajor;
using jobs::kProtocolMinor;
namespace protocol = jobs::v1;

constexpr std::string_view kProResHalfPreset{"video-editor.proxy.prores-half.v1"};
constexpr std::string_view kFfv1HalfPreset{"video-editor.proxy.ffv1-half.v1"};

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

[[nodiscard]] bool run_proxy(const protocol::JobSpec& spec, const EventSink& sink) {
  ParsedProxyRequest parsed;
  std::string validation_error;
  if (!parse_proxy_request(spec, parsed, validation_error)) {
    return emit_invalid(spec, sink, validation_error);
  }

  std::stop_source internal_stop;
  double last_progress = 0.0;
  bool sink_available = true;
  const auto progress = [&](const proxy::Progress& update) {
    if (!sink_available) {
      return;
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

} // namespace

bool dispatch_job(const protocol::JobSpec& spec, const EventSink& sink) {
  if (!sink) {
    return false;
  }
  if (!sink(event_for(spec, protocol::JOB_STATE_ACCEPTED, 0.0, "accepted"))) {
    return false;
  }
  if (spec.kind() == protocol::JOB_KIND_PROBE) {
    return run_probe(spec, sink);
  }
  if (spec.kind() == protocol::JOB_KIND_PROXY) {
    return run_proxy(spec, sink);
  }
  auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "unsupported");
  fail(event, "unsupported-job", 0, "This worker does not implement that job yet.",
       "only JOB_KIND_PROBE and JOB_KIND_PROXY are enabled in this worker");
  return sink(event);
}

bool reject_unavailable_cancellation(const protocol::CancelJob& request, const EventSink& sink) {
  if (!sink) {
    return false;
  }
  protocol::JobSpec spec;
  spec.set_job_id(request.job_id());
  auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "cancellation-unavailable");
  fail(event, "unsupported-cancellation", 0,
       "This worker cannot cancel a job after it has started.",
       "the stdin/stdout framing loop dispatches jobs synchronously and cannot read CancelJob "
       "until the active job has finished");
  return sink(event);
}

} // namespace video_editor::workers
