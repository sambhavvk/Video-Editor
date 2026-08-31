// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/job_service/cancellation_registry.h"
#include "video_editor/job_service/protocol.h"

#include <functional>

namespace video_editor::transcription {
class TranscriptionService;
}

namespace video_editor::workers {

using EventSink = std::function<bool(const jobs::v1::WorkerEvent&)>;

struct DispatchDependencies final {
  // Borrowed for the duration of dispatch_job. A null service is deliberate:
  // builds without whisper.cpp report BackendUnavailable truthfully.
  video_editor::transcription::TranscriptionService* transcriber{nullptr};
};

// Runs one job synchronously. The sink is called in protocol order and must be
// safe to call for the duration of this function. A false return requests an
// internal stop; it is not the wire-protocol CancelJob mechanism.
[[nodiscard]] bool dispatch_job(const jobs::v1::JobSpec& spec, const EventSink& sink);
[[nodiscard]] bool dispatch_job(const jobs::v1::JobSpec& spec, const EventSink& sink,
                                DispatchDependencies& dependencies);
[[nodiscard]] bool dispatch_job(const jobs::v1::JobSpec& spec, const EventSink& sink,
                                DispatchDependencies& dependencies,
                                jobs::CancellationRegistry& registry);

// Applies CancelJob to the active registry entry. Returns true when the job
// was running and cancellation was shared. Unknown jobs emit a typed failure.
[[nodiscard]] bool handle_cancel_job(const jobs::v1::CancelJob& request,
                                     jobs::CancellationRegistry& registry, const EventSink& sink);

} // namespace video_editor::workers
