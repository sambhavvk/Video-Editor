// SPDX-License-Identifier: MPL-2.0
#include "video_editor/job_service/framing.h"
#include "video_editor/job_service/protocol.h"
#include "video_editor/media_codec/probe.h"

#include <iostream>
#include <string>

namespace {

using video_editor::jobs::kProtocolMajor;
using video_editor::jobs::kProtocolMinor;
namespace protocol = video_editor::jobs::v1;

protocol::WorkerEvent event_for(const protocol::JobSpec& spec, const protocol::JobState state,
                                const double progress, const std::string& phase) {
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

bool emit(const protocol::WorkerEvent& event) {
  return video_editor::jobs::write_frame(std::cout, event).ok;
}

void fail(protocol::WorkerEvent& event, const std::string& category, const int native_code,
          const std::string& user_message, const std::string& diagnostic) {
  event.mutable_event()->set_state(protocol::JOB_STATE_FAILED);
  protocol::JobError* error = event.mutable_event()->mutable_error();
  error->set_category(category);
  error->set_native_code(native_code);
  error->set_user_message(user_message);
  error->set_diagnostic(diagnostic);
}

bool run_probe(const protocol::JobSpec& spec) {
  if (spec.input_uris_size() != 1) {
    auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "validating");
    fail(event, "invalid-argument", 0, "The probe job needs one media file.",
         "input_uris must contain exactly one item");
    return emit(event);
  }
  if (!emit(event_for(spec, protocol::JOB_STATE_RUNNING, 0.1, "probing"))) {
    return false;
  }

  const auto result = video_editor::media::probe(spec.input_uris(0));
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
  return emit(event);
}

bool run_job(const protocol::JobSpec& spec) {
  if (!emit(event_for(spec, protocol::JOB_STATE_ACCEPTED, 0.0, "accepted"))) {
    return false;
  }
  if (spec.kind() == protocol::JOB_KIND_PROBE) {
    return run_probe(spec);
  }
  auto event = event_for(spec, protocol::JOB_STATE_FAILED, 0.0, "unsupported");
  fail(event, "unsupported-job", 0, "This worker does not implement that job yet.",
       "only JOB_KIND_PROBE is enabled in the CPU foundation worker");
  return emit(event);
}

} // namespace

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  while (true) {
    protocol::WorkerRequest request;
    const auto result = video_editor::jobs::read_frame(std::cin, request);
    if (result.status == video_editor::jobs::ReadStatus::EndOfStream) {
      break;
    }
    if (!result.ok) {
      std::cerr << result.message << '\n';
      return 2;
    }
    if (!video_editor::jobs::compatible(request)) {
      std::cerr << "incompatible worker protocol" << '\n';
      return 3;
    }
    if (request.has_shutdown()) {
      break;
    }
    if (request.has_start() && !run_job(request.start().spec())) {
      return 4;
    }
  }
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}

