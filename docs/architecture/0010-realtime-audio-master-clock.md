<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0010: Realtime audio playback owns the master clock

- **Status:** accepted
- **Date:** 2026-08-09
- **Owners:** Core/Media, Desktop/Product, and Quality/Platform

## Context

The existing timeline audio renderer is deterministic but performs allocation, FFmpeg decode,
resampling, seeking, and filesystem I/O. None of that work is safe in an audio-device callback.
Timer-driven video transport also accumulates drift and cannot satisfy the two-hour A/V requirement.
The editor needs one playback clock whose behavior remains exact across pause, resume, seek,
underrun, device absence, and revision changes.

## Decision

- Realtime playback has a fixed beta format of 48 kHz, two-channel, interleaved float32 at the device
  boundary. `PlaybackAudioProvider` renders exact epoch-tagged sample ranges away from the callback.
- A worker keeps a bounded single-producer/single-consumer ring pre-rendered. Provider/decode work,
  allocation, locks, condition waits, logging, project access, disk I/O, and error formatting are
  forbidden in the callback.
- `AudioOutputDevice` receives a raw function pointer plus context rather than an allocating callable.
  The callback only reads already-interleaved samples, fills any shortage with deterministic zero,
  and updates atomic counters.
- The submitted counter records the end of the most recently submitted output buffer. It is useful
  for diagnostics, but is not an audible clock. The canonical playback master clock subtracts a
  conservative estimated device/output-buffer latency from that submitted position, clamps at the seek
  origin, and
  exposes the remaining latency as `clock_uncertainty_frames`. When a per-device calibration exists in
  QSettings, the subtracted term uses that measured offset instead of the live backend estimate;
  uncertainty becomes the residual between estimate and calibration (never zero). Video derives
  requested timeline time from this conservative position; a UI elapsed timer must not independently
  advance the playhead.
- Pause stops clock advancement without changing its absolute position. Resume continues from that
  sample after the ring is ready. Without calibration, latency is an estimate rather than a hardware
  timestamp, so the reported playhead deliberately carries explicit uncertainty. Calibration reduces
  but does not eliminate that uncertainty.
- Seek is a generation boundary: stop device and worker, increment the request epoch, reset ring and
  producer/consumer cursors, set the absolute sample counter, prefill, then restart only if transport
  was playing. Blocks from older epochs cannot enter the new ring.
- Device xrun events and zero-filled underrun frame counts are atomic diagnostics. They can be
  sampled and presented outside the callback without logging or allocating inside it.
- A deterministic manual/fake output device exercises the same callback and clock contract without
  hardware. If no physical adapter is compiled or available, the engine can use this software/manual
  path for tests and controlled fallback; it must not claim that the user can hear audio.
- A miniaudio 0.11.25 adapter is optional at compile time. The repository does not fetch or vendor it
  implicitly. Missing miniaudio leaves the core/fake contract buildable and must be reported through
  capabilities/diagnostics rather than a late device-start failure.
- Playback device state is ephemeral and never enters `.veproj`. The immutable project revision,
  exact timeline sample ranges, and clip/track audio commands remain authoritative.
- Desktop controls enqueue versioned intents through `AsyncRealtimeAudioPlayback`; one dedicated
  non-callback thread alone calls the synchronous controller. Pending transport requests coalesce
  into the newest complete intent while preserving an explicit Start/Seek position through a later
  Pause or Resume. Any newer accepted intent cancels an active Start/Seek/Resume prefill.
  Cancellation clearing and dequeue are serialized with enqueue, preventing a newer cancellation
  from being lost between command admission and execution. Only the newest intent version may
  publish effective state or an error; Stopped/Failed controllers are reconciled through a
  resumable exact paused position before the requested target is applied.

## Consequences

- Decode-ahead can absorb normal media latency while keeping the callback bounded and nonblocking.
- Audible underrun produces silence but does not make video stall or the clock drift backward;
  diagnostics retain the failure signal for adaptive quality and release tests.
- Stop/revision/seek code must join or quiesce the producer before reusing its ring storage. Correct
  lifecycle control is more important than minimizing a transport transition.
- Realtime playback may use provider/session optimizations behind the exact range contract, but it
  cannot reuse stale revision or resampler history.
- Offline export remains self-contained and deterministic; it does not depend on a device or the
  realtime ring.

## Verification

The deterministic device must cover prefill, exact callback output, pause/resume, seek epoch
invalidation, end-of-sequence behavior, underrun zero-fill, xrun/underrun diagnostics, and the sample
counter under irregular callback sizes. Stress tests verify the callback performs no allocation or
locking and exercise producer/consumer wraparound and shutdown races. Async-control tests cover
immediate Stop during blocked Start, rapid Seek then Pause, bounded-queue coalescing, stale-completion
suppression, typed failure publication, and concurrent versioned seek producers under ThreadSanitizer.

The desktop adapter binds an immutable timeline snapshot for each forward 1× playback session,
uses 24,000-frame render requests, prefills 48,000 frames, and bounds the ring at 192,000 frames.
It drives the playhead from latency-compensated `sample_counter()` rather than the submitted
counter, stops playback when the revision or project changes, and reports silent timer fallback and
underruns outside the callback. Direct core controls remain synchronous. `AsyncRealtimeAudioPlayback`
adds a bounded, versioned command queue and dedicated serialized control thread so a GUI can enqueue
start/pause/resume/seek/stop without blocking on prefill/provider work. The desktop uses command
receipts and versions, holds timer-driven video during pending start/seek, polls effective state from
its precise timer, and adopts the audio master only after successful completion. Stop is enqueued on
edits and project replacement. Callback-thread constraints do not change.

Public beta additionally requires real-device latency calibration evidence, a one-hour zero-xrun
run, and two-hour A/V drift below 10 ms on the supported Windows and Linux matrix. The desktop now
stores a measured per-device latency offset in QSettings and applies it on playback start; the 10 ms,
one-hour, and two-hour gates remain physical lab requirements, not CI fake-device claims. Reverse/non-1×
audible transport and adaptive buffer policy also remain product work; the connected desktop path
is not yet a release-grade audible-preview claim.
