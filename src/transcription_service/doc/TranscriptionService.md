<!-- SPDX-License-Identifier: MPL-2.0 -->

# TranscriptionService and model-management API

Header: `video_editor/transcription_service/transcription_service.h`

Namespace: `video_editor::transcription`

## Purpose

`TranscriptionService` coordinates three injected boundaries: a `ModelManager`, an `AudioDecoder`,
and a `TranscriptionBackend`. The production worker supplies the FFmpeg decoder and optional
`whisper.cpp` backend. Tests can replace all three and therefore require neither a model download nor
live media decoding.

The service is synchronous and intended for a worker thread or worker process. It performs file I/O,
decoding, allocation, hashing, and inference and must never run on the Qt thread or audio-device
callback.

## Manifest and wire values

The generated `model_manifest.h` binds the compiled service to the dependency pins in
`cmake/DependencyVersions.cmake`. `default_model_descriptor()` exposes the supported multilingual
base model's ID, filename, download URL, digest algorithm/value, and exact byte length. The service
uses the worker protocol's `TranscribeOptions` and `TranscriptionResult` messages as its typed option
and result values.

`validate_options` accepts schema v2, model `base`, `auto` or a supported two-letter language code,
a bounded thread count, and either no source window or a non-negative start with positive duration.
An unsupported schema, model, language, or range fails before media or model I/O.

## ModelManager

`ModelManager(cache_directory, fetcher, descriptor)` owns paths and a copy of the descriptor; it
borrows `ModelFetcher` for its own lifetime.

| Member | Contract |
| --- | --- |
| `verify(stop_token)` | Checks the installed path's exact byte length and digest, cooperatively stopping between bounded read chunks. It never performs network I/O. |
| `model_path()` | Returns the expected cache path. Existence or validity is not implied. |
| `ensure(stop_token)` | Returns a verified installed model, or asks the injected fetcher to write a unique staging file, verifies it, and atomically installs it. |

Cancellation, download failure, short content, checksum mismatch, and install failure return typed
errors and remove the staging file. A valid installed model is reused without calling the fetcher.
The worker uses an unavailable fetcher deliberately; desktop-initiated download is the only beta
network boundary.

## AudioDecoder and backend

`AudioDecoder::decode` produces owned mono float32 samples at 16 kHz. The FFmpeg implementation
opens an absolute original-media path, selects the best audio stream, seeks with preroll for a
requested source window, flushes seek state, trims to the exact 16 kHz sample interval, drains
packet/frame boundaries, flushes the decoder/resampler, and checks the stop token between bounded
operations.

`TranscriptionBackend::capabilities` reports whether inference is compiled and whether that exact
build asserts Vulkan provenance. It cannot prove that a particular inference used Vulkan.
`transcribe` consumes the verified model and conformed audio and always requests timed token output.
The service rejects malformed UTF-8/NUL, oversized text/metadata, invalid probabilities, and
non-positive, overlapping, unsorted, or decoded-range-exceeding word records. A build without
`whisper.cpp` uses the unavailable backend and reports `BackendUnavailable`; it never pretends to
transcribe.

## TranscriptionService

The constructor is `TranscriptionService(ModelManager& models, AudioDecoder& decoder,
TranscriptionBackend& backend)`.

The service borrows all three collaborators. `transcribe(input, options, stop_token, progress)`
validates input and options, ensures the model, decodes audio, invokes inference, and attaches the
model/digest/capability plus decoded source-window metadata to the typed result. Word times returned
to the controller are source-absolute even when only a window was decoded. Progress is monotonic in
the service's normalized 0–1 range. A requested stop returns `Cancelled` at the next cooperative
boundary.

The beta desktop obtains stronger cancellation by terminating the dedicated worker process. No raw
audio, model bytes, or frames cross the Protobuf protocol.

## Errors and ownership

`ErrorCode` distinguishes invalid input/options, cancellation, decode failure, model availability,
download/size/digest failures, backend absence, and inference failure. `Result<T>` owns either its
value or error. Callback and collaborator references must outlive a synchronous call; returned audio,
messages, strings, and paths are owned values.

AI assistance has been used to create this output.
