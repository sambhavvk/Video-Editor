// SPDX-License-Identifier: MPL-2.0
#include "video_editor/job_service/cancellation_registry.h"
#include "video_editor/job_service/framing.h"
#include "video_editor/job_service/protocol.h"
#include "video_editor/transcription_service/transcription_service.h"
#include "video_editor/workers/job_dispatch.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stop_token>
#include <thread>

namespace protocol = video_editor::jobs::v1;

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  const char* configured_model_dir = std::getenv("VIDEO_EDITOR_TRANSCRIPTION_MODEL_DIR");
  const std::filesystem::path model_dir =
      configured_model_dir == nullptr ? std::filesystem::current_path() / ".video_editor_models"
                                      : std::filesystem::path(configured_model_dir);
  auto fetcher = video_editor::transcription::make_unavailable_model_fetcher();
  video_editor::transcription::ModelManager models(model_dir, *fetcher);
  auto decoder = video_editor::transcription::make_ffmpeg_audio_decoder();
  auto backend = video_editor::transcription::make_default_backend();
  video_editor::transcription::TranscriptionService transcriber(models, *decoder, *backend);
  video_editor::workers::DispatchDependencies dependencies{.transcriber = &transcriber};
  video_editor::jobs::CancellationRegistry registry;
  std::mutex stdout_mutex;
  std::atomic<int> exit_code{0};
  std::jthread job_thread;
  const video_editor::workers::EventSink sink = [&](const protocol::WorkerEvent& event) {
    std::scoped_lock lock(stdout_mutex);
    return video_editor::jobs::write_frame(std::cout, event).ok;
  };

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
    if (request.has_cancel()) {
      if (!video_editor::workers::handle_cancel_job(request.cancel(), registry, sink)) {
        exit_code = 4;
      }
      continue;
    }
    if (request.has_start()) {
      if (job_thread.joinable()) {
        job_thread.join();
      }
      const protocol::JobSpec spec = request.start().spec();
      job_thread = std::jthread([&, spec](const std::stop_token) {
        if (!video_editor::workers::dispatch_job(spec, sink, dependencies, registry)) {
          exit_code = 4;
        }
      });
    }
  }

  if (job_thread.joinable()) {
    job_thread.join();
  }
  google::protobuf::ShutdownProtobufLibrary();
  return exit_code.load();
}
