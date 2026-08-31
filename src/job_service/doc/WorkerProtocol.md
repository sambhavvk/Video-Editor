<!-- SPDX-License-Identifier: MPL-2.0 -->

# Versioned worker protocol

Headers: `video_editor/job_service/protocol.h`, generated `job_service.pb.h`

Namespace: `video_editor::jobs`

`WorkerRequest` and `WorkerEvent` use protocol major/minor values checked by `compatible`. Each
serialized message is framed by a four-byte little-endian payload length and is rejected before
allocation above `kMaximumFrameBytes`. Job IDs are canonical UUID identifiers. The cancellation
registry shares stop state for an active job; the worker host reads `CancelJob` on stdin while
dispatch runs on a worker thread and combines the registry token with internal stop sources.
Unknown idle cancellations return an explicit `job-not-found` failure.

`JobSpec.options` remains opaque at the generic boundary. A `JOB_KIND_TRANSCRIBE` dispatcher parses
it as schema-v2 `TranscribeOptions`, rejects unknown fields, and returns a serialized
`TranscriptionResult` in the terminal event. The typed contract carries the pinned
model/language/thread/GPU request plus an exact optional source range. Timed words are mandatory in
the result; the retained `word_timestamps` request field is wire-compatible but cannot weaken that
contract. Results carry ordered word text, source-absolute centisecond timing, probability, detected
language, model/digest, backend, requested range, and capability metadata. Raw model, audio, and
video data are never protocol payloads.

Independent framed messages and generated protobuf values own their storage. Callers must not retain
references into a message after mutation or destruction.

AI assistance has been used to create this output.
