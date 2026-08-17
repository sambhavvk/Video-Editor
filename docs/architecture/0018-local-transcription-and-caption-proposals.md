<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0018: Local transcription, timed captions, and reviewable edit proposals

**Status:** Accepted  
**Date:** 2026-08-14

## Context

The beta needs useful local transcription without making the editor, project format, or ordinary
caption editing depend on an account or network connection. Transcription is expensive and consumes
untrusted media plus a large optional model, so it must not run in the audio callback or on the Qt
thread. Generated captions and silence edits are suggestions: silently changing the authoritative
timeline would violate the product rule that automatic actions are previewable, cancelable, and
undoable.

Existing foundations already provide immutable timeline revisions, exact rational time, 48 kHz
timeline audio rendering, versioned worker frames, restartable worker executables, SRT/WebVTT
exchange, and deterministic caption burn-in. This decision connects those foundations without
creating a second timeline or caption model.

## Decision

### Process and network boundaries

- `whisper.cpp` is an optional, exactly pinned backend of `transcription_service`. A build without
  it remains valid and reports a typed `BackendUnavailable` result; it must not advertise local
  transcription as usable.
- The desktop launches a fresh `video_editor_worker_host` process for each transcription job. The
  request and progress/result events use the existing length-delimited Protobuf worker protocol.
  Closing or terminating that one process is the truthful cancellation and crash-containment
  boundary while the generic worker host remains synchronously dispatched.
- The request carries the selected clip's authoritative source window. The worker seeks with
  preroll, decodes and exactly trims that window to mono float32 at 16 kHz through FFmpeg, invokes
  `whisper.cpp`, and returns source-absolute typed word records. Raw audio and frames never cross
  the worker protocol.
- Model download is explicitly initiated in the desktop. The worker has no network implementation.
  A declared length that differs from the pin is rejected; streaming aborts before writing beyond
  the pinned byte ceiling. Download bytes are staged, size-checked while arriving, and hashed away
  from the Qt thread with cooperative cancellation before atomic installation. A failed, canceled,
  short, oversized, or digest-mismatched download is discarded without replacing an already
  verified model.
- Local editing, caption import/export, project recovery, and export continue to work without the
  model or an internet connection. No media, transcript, or project data is uploaded.

### Canonical caption data

- The snapshot schema advances to v3 for `CaptionWord`, word provenance, and renderer-actionable
  caption style fields. The SQLite project-store envelope remains schema v2 because its tables do
  not change; journal entries already record their independent payload schema.
- Each word has a stable entity ID, UTF-8 text, an exact half-open timeline range, and a finite
  probability. Words are ordered, non-overlapping, and contained in their caption cue. Caption-level
  provenance records whether timing came from import, local transcription, or a later user edit and
  identifies the model/digest once for the cue.
- Caption styling uses the same canonical state for the panel, project snapshot, preview, and export
  burn-in. Supported fields are font family request, size, text/background colors, emphasis,
  horizontal alignment, normalized vertical position and safe margin, plus outline width/color.
  The deterministic bitmap reference renderer may substitute its built-in glyphs for an unavailable
  font; the UI and documentation must state that limitation.
- Schema-v1 and schema-v2 snapshots remain readable with the pre-v3 caption defaults. A payload that
  declares an older schema while carrying v3 fields is rejected instead of being silently
  reinterpreted.

### Review and application

- Timed words are mandatory canonical output. The legacy request boolean remains wire-compatible,
  but the adapter always enables whisper token timestamps and rejects empty, malformed UTF-8,
  unbounded, zero-duration, overlapping, unsorted, or out-of-range backend records.
- Transcription output is converted to ordinary proposed captions with exact source-to-timeline
  mapping, including trimmed, rate-adjusted, and reversed clips. Timed reflow may group words into
  readable cues, but it does not discard word timing.
- Transcript navigation resolves cue and word identity to exact timeline time. Search remains a
  presentation query and never mutates the project.
- Measured-silence proposals are derived incrementally from bounded chunks of exact 48 kHz audio
  rendered from an immutable snapshot. Runs merge across chunk boundaries and proposed cuts are
  inset by 5 ms. Transcript filler/deletion proposals are labelled separately; transcript gaps are
  not represented as measured silence.
- A proposal owns its base revision and review items. The user can select items, inspect their
  ranges, apply them, or discard them. Application submits complete deterministic caption and track
  change sets with the proposal revision as `expected_revision`.
- Accepted caption and timeline changes commit as one atomic edit batch and one undo step. If the
  project revision or source selection changed, application fails as stale and the proposal must be
  regenerated. Partial application after a validation failure is forbidden.

## Consequences

The editor gains a useful offline workflow while keeping optional inference isolated from playback
and authoritative project state. Timed words, style, and proposals can be tested without a model or
network by injecting decoder, backend, download, and process boundaries. One-process-per-job has
more startup overhead than a persistent worker, but gives deterministic cancellation and worker
death recovery for the beta.

This decision does not provide cloud transcription, speaker diarization, arbitrary subtitle stream
encoding, a production font-shaping engine, native worker multiplexing, or generative transcript
rewriting. These remain separate future decisions.

## Verification

Required coverage includes model size/digest and atomic-install failures; worker protocol validation,
progress, cancellation, crash, and unavailable-backend behavior; FFmpeg decode and flush boundaries;
v1/v2 compatibility plus v3 deterministic round trips; timed reflow/search/navigation; exact
48 kHz silence boundaries; proposal determinism and stale-revision rejection; atomic apply,
undo/redo, and save/reopen; caption-style preview/export parity; and Qt accessibility and end-to-end
review workflows. Tests must use local fixtures and injected downloads, never the live model URL.

AI assistance has been used to create this output.
