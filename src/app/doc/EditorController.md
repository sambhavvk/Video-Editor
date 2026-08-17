<!-- SPDX-License-Identifier: MPL-2.0 -->

# EditorController caption and transcription orchestration

Header: `src/app/editor_controller.hpp`

Namespace: `video_editor::app`

## Role

`EditorController` owns the current `TimelineEditor`, local working project store, runtime media
registries, desktop signal wiring, and asynchronous job state. Qt widgets remain presentation-only;
caption/transcription operations are converted into immutable snapshots and revision-checked edit
commands here.

## Model acquisition and worker boundary

Model verification runs away from the GUI thread. Download begins only after the panel emits an
explicit request. The controller writes into an owned unique staging directory, rejects a known
length mismatch, aborts before streamed bytes exceed the pin, presents byte progress, and installs
only an exact-length/digest match. Cancellation covers both network transfer and digest work;
terminal paths and destruction remove partial staging data. Models are rebuildable application data
outside `.veproj`.

`startTranscription` requires one selected media clip with authoritative original audio. It does no
large checksum work on the Qt thread; the worker authoritatively verifies the installed model and
returns a typed failure if it is missing or corrupt. The controller captures the base project
revision and the clip's source/timeline/rate/reverse
mapping, sends a conservatively rounded source window rather than decoding the entire asset, creates
one typed `JOB_KIND_TRANSCRIBE` request, and launches a fresh worker host through
`WorkerHostSession`. Requests and events are four-byte little-endian length-prefixed Protobuf frames
bounded by `jobs::kMaximumFrameBytes`. The worker has no network implementation. Cancellation
terminates that job process; abnormal termination or malformed results cannot mutate the project.

Proxy generation and creator export use the same process boundary. `generateProxy` sends
`JOB_KIND_PROXY` with one absolute source path, an absolute cache destination, and the resolved
FFV1 or ProRes half-resolution preset. `startVideoExport` serializes the current project revision
to a unique temp checkpoint and sends `JOB_KIND_EXPORT` with typed `ExportOptions`. Deliver-panel
progress comes from `RUNNING` events, including the hardware-fallback phase. In every case
`WorkerHostSession::cancel()` kills the host; worker death or a missing terminal `SUCCEEDED` is a
failure that leaves the destination unchanged and does not register a complete proxy. The temp
export checkpoint is deleted when the process finishes.

## Timed results and review

Worker centisecond timing is clamped to the selected source range, mapped into exact timeline time,
ordered in playback direction, and reflowed while retaining stable word timing. The resulting
captions are proposals, not project state.

The controller separately renders the selected immutable timeline range at exact 48 kHz in bounded
chunks and runs incremental measured-silence detection on a worker thread. Standalone conservative
filler words are labelled separately. Review items own selection state and the captured base
revision; measured silence starts selected and transcript fillers start unselected.

`applyCaptionReview` merges selected cut ranges, asks `caption_service` for deterministic complete
track replacements, and submits caption additions plus timeline cuts through one atomic
`TimelineEditor::applyBatch`. Any stale revision, locked/unsupported track state, transition
conflict, invalid result, cancellation, or discard leaves the authoritative project unchanged.

## Lifetime and threading

The controller is GUI-thread-affine. Network replies and `WorkerHostSession` / `QProcess` objects
are QObject children. Audio analysis, model verification, import, preview, and cache work execute
off the GUI thread and return owned outcomes through Qt watchers. Proxy, export, and transcription
run in a restartable worker-host process. Destruction kills those processes and waits for owned
futures before releasing the project and registries.

AI assistance has been used to create this output.
