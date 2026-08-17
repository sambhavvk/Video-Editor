<!-- SPDX-License-Identifier: MPL-2.0 -->

# Project codec API reference

Header: `video_editor/project_codec/project_codec.h`

Namespace: `video_editor::project_codec`

The codec is the complete-project protobuf boundary. Generated protobuf classes are private and do
not form part of the application API.

## Version constants

`kCurrentSchemaVersion` is 3 and is written by `serialize_project`. `kMinimumReaderVersion` is 1.
The reader accepts declared versions 1, 2, and 3, rejects future schemas, and validates the embedded
minimum-reader declaration independently.

Schema v2 adds canonical title payloads and sequence-owned transitions. It also carries track
output visibility and insertion targeting as presence-aware optional fields. Older v2 payloads that
omit those additive fields decode both as true, matching the canonical model defaults. A schema-v1
title clip has no title payload, so the backward reader creates the default `Title` value and uses
the clip name as its text. A declared-v1 document containing v2 fields is rejected rather than
reinterpreted.

Schema v3 adds stable caption words, exact ranges/probabilities, word provenance/model identity, and
alignment/vertical/safe-margin/outline style fields. Genuine v1/v2 payloads upgrade to canonical
pre-v3 defaults. A declared older payload carrying v3 fields is rejected. The SQLite project-store
envelope remains schema v2; current journal entries use type `project.snapshot.v3` with payload
schema version 3.

## `serialize_project`

`serialize_project(const edit::Project&)` returns owned canonical `ProjectBytes`. It validates the
entire model before encoding and throws `CodecException` on invalid model state, unsupported values,
or an oversized result. Calls share no mutable global state and may run concurrently on independent
immutable projects.

## `deserialize_project`

`deserialize_project(span<const byte>)` returns either one completely validated owned project or a
`CodecError`. It never returns partial state. The input bytes remain caller-owned and need only stay
valid for the duration of the call.

## Errors

`CodecError` provides a stable `CodecErrorCode`, a diagnostic message, and a model-oriented field
path. `CodecException::error()` borrows the exception's stored error and remains valid for the
exception lifetime.

Snapshots are bounded by `kMaximumSnapshotBytes`. Unknown edit effects round-trip through explicit
opaque model fields; unknown protobuf fields at a declared current schema fail closed.
Clip/marker/gap selection is presentation state and is not encoded.

AI assistance has been used to create this output.
