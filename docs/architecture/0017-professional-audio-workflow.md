<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0017: Professional track audio, DSP, meters, and normalization

- **Status:** accepted
- **Date:** 2026-08-13
- **Owners:** Core/Media and Desktop/Product

## Context

The realtime engine already established a fixed 48 kHz stereo callback boundary and audio-master
clock, while the timeline renderer provided exact originals-only clip mixing. Beta audio authoring
also needs persistent track controls, deterministic processing, useful live feedback, reviewable
loudness normalization, and explicit device selection without putting UI, decoding, or DSP work in
the device callback.

## Decision

### Authoritative track state

- Audio tracks store gain in the closed range −96 through +24 dB and constant-power pan in −1
  through +1. Typed commands are revision-checked, coalescible, undoable, and schema-v2 persistent;
  the schema-v1 reader rejects smuggled v2 fields and otherwise supplies neutral defaults.
- Track effects use the existing typed/versioned `Effect` model. The supported canonical types are
  parametric EQ, compressor, dialogue noise reduction, and limiter. Known parameter ranges are
  validated before a revision is published. Unknown future effects remain round-trippable and
  disabled according to the general effect contract.
- Audio render mixes clips, applies track gain/pan, then runs a stateful chain in the fixed order
  EQ → compressor → dialogue noise reduction → limiter. State persists across consecutive blocks
  on the render/pre-render worker and never enters the audio callback.

### Metering and loudness

- The callback performs bounded arithmetic and atomic telemetry only. It publishes master peak/RMS
  directly and copies audio into a preallocated bounded SPSC queue. A dedicated worker owns
  libebur128 and publishes versioned momentary, short-term, and integrated EBU-R128 readings with
  explicit validity, staleness, analyzed-frame, and dropped-block state. Queue overload never
  blocks the device callback and makes loudness stale until reset.
- Per-track peak/RMS taps run after track gain/pan and DSP on the pre-render worker. A bounded
  immutable history records exact sample ranges and stable track IDs; the desktop selects the
  range containing the latency-compensated audio-master position instead of showing a future
  decode-ahead block. Muted/non-solo, removed, or uncovered tracks are inactive or stale.
- Authoritative normalization runs on a background worker over one immutable timeline revision
  using libebur128. Its persisted creator target is editable from −24 through −9 LUFS. Apply is
  rejected when the revision or target generation is stale or any contributing audio track would
  leave the canonical gain range, and a valid proposal publishes one atomic undo command batch.
- No invisible limiter or per-track clamp is introduced by normalization. The reviewed proposal is
  the edit that is applied.

### Devices and recovery boundary

- Device enumeration produces stable opaque IDs and human-readable names off the Qt thread. The
  persisted selected ID is passed into miniaudio when realtime playback opens.
- On Linux builds with miniaudio, an open realtime output device registers miniaudio
  `notificationCallback` handlers for stop, reroute, and interruption events. Notifications are
  marshaled off the backend thread onto the Qt main thread and trigger an immediate off-UI-thread
  device refresh. A 7.5 s backup poll remains while notifications are live; idle or unavailable
  backends keep the one-second poll.
- Loss of the selected endpoint or current system default pauses the audio master safely; return
  records a persistent recovery intent and retries reopen only after the serialized stop settles. A
  later pause/stop cancels that intent, preventing stale playback from restarting. The empty stable
  ID selects **System default**.
- `AudioDeviceRecovery` remains the dependency-free backend coordinator. Polling and miniaudio
  notifications together bound hot-plug detection latency; neither path opens or closes devices from
  realtime callbacks.
- An unavailable or failed selected device retains the existing explicit silent timer fallback;
  it never changes the project revision.

## Consequences

The same track controls and processing are heard in realtime pre-render and deterministic export,
and loudness adjustment remains a visible ordinary edit. The device callback still performs no
allocation, locking, decoding, filesystem access, Qt work, libebur128 analysis, or stateful DSP.
Arbitrary buses, time-stretch, Pulse/PipeWire idle hot-plug subscription, and the supported
physical-device endurance matrix remain explicit follow-up work. Per-device output latency
calibration is stored in QSettings and applied on playback start; residual uncertainty remains.

## Required verification

- Track gain/pan command, persistence, backward-reader, stale revision, range, undo/redo, and
  coalescing tests.
- DSP parameter validation, known/unknown effect behavior, ordered-chain fixtures, block-partition
  parity, state continuity, and sanitizer coverage.
- Callback allocation/lock boundary; bounded loudness-queue overload/reset/shutdown under normal,
  ASan/UBSan, and TSan; sample-range track meters; stale/unsafe proposal rejection; target-generation
  invalidation; and atomic normalization apply tests.
- Stable-ID enumeration, selected/default loss and return, delayed-stop recovery, canceled recovery,
  and system-default selection tests, plus the supported physical-device matrix tests.
- Accelerated one-hour zero-xrun and two-hour drift tests, followed by real-time physical endurance
  evidence before public beta.
