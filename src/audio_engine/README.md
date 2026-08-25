<!-- SPDX-License-Identifier: MPL-2.0 -->

# Audio engine

The beta audio engine owns the fixed 48 kHz stereo float32 playback clock,
bounded decode-ahead, callback-safe device boundary, DSP primitives, meters, and
lock-free interleaved SPSC ring. Timeline decoding remains behind the narrow
`PlaybackAudioProvider` interface so this module does not depend on FFmpeg, Qt,
the edit model, or `TimelineAudioRenderer` types.

## Realtime playback contract

`RealtimeAudioPlayback` runs provider requests on a `std::jthread`. Each Ready
result must cover the requested absolute sample range and current seek epoch
exactly. The worker may allocate, lock, decode, and perform I/O, then converts
the planar block to interleaved samples and writes it into the bounded ring.
Backpressure is limited by `ring_capacity_frames`; no raw audio accumulates
outside the current provider block and ring.

The device callback performs only these operations:

- zero the caller-owned output span;
- read available interleaved frames from `SpscAudioRing`;
- update always-lock-free atomic counters.

It never invokes the provider and performs no allocation, locking, decoding,
disk I/O, logging, or device control. Missing frames remain zero and increment
one xrun event plus the exact underrun-frame count. While Playing,
`submitted_sample_counter()` advances by every frame requested by the device,
including underrun silence. That counter is the end of the submitted buffer and
is explicitly **not** the video/audible clock.

The canonical `sample_counter()` is a conservative playback position. It
subtracts the greater of the current callback period and the backend's
callback-safe output-latency estimate from the submitted position, clamped at
the current seek origin. When a per-device calibration is supplied,
`sample_counter()` subtracts that measured offset instead and
`clock_is_estimated` is false; `clock_uncertainty_frames` is the residual
between the live buffer estimate and the calibration (never zero). Without
calibration, `PlaybackDiagnostics` publishes both positions,
`estimated_output_latency_frames`, and an equal `clock_uncertainty_frames` with
`clock_is_estimated = true`. This prevents video from following the unplayed end
of a newly submitted buffer. Calibration is not evidence for the beta's
less-than-10-ms A/V lab gate. `clock_seconds()` is diagnostic convenience only. Paused callbacks return
silence without advancing either position.

`pause()`, `seek()`, and `stop()` first make playback non-playing, then rely on
`AudioOutputDevice::stop()` synchronously joining callbacks before stopping the
worker or resetting the ring. A seek increments the epoch and resets both render
and playback cursors, preventing queued samples from an older revision/position
from escaping. Providers must honor their stop token so control operations can
join promptly. EndOfStream stops new provider demand; device-requested time can
continue advancing until the controller stops playback.

Direct `start()`, `pause()`, `resume()`, `seek()`, and `stop()` calls are
synchronous and may wait for provider prefill, a worker join, or device control.
GUI code uses `AsyncRealtimeAudioPlayback`: request methods enqueue into a
bounded queue and return a strictly increasing receipt version immediately,
while one dedicated non-callback thread serializes the direct controller.
Diagnostics distinguish requested state from the latest effective state and
publish pending/succeeded/failed status plus the typed error for the newest
intent. A completion can update effective state only if its version is still
current.

All current pending transport requests coalesce into the newest intent. An
explicit Start/Seek position is retained when the newest request is Pause or
Resume, allowing the reconciler to establish that exact position without first
starting stale audio. Any newer accepted intent cooperatively cancels an active
Start, Seek, or Resume prefill, and the cancellation clear/dequeue operation is
serialized with enqueue so a request cannot be lost in the handoff gap. A
cancelled prefill settles the direct controller in a safe paused state; the
serialized successor then reconciles Playing, Paused, Stopped, or Failed to the
latest target. Facade destruction also cancels provider work before joining the
control thread. Provider cancellation remains a required part of the adapter
contract.

The default configuration buffers two seconds, renders 20 ms blocks, and
prefills 100 ms. Callers may select larger bounded blocks for request-local
decoders; for example, a 192,000-frame ring, 24,000-frame render block, and
48,000-frame prefill provide four seconds / 500 ms / one second respectively.

## Devices and fallback

`AudioOutputDevice` accepts a raw function-pointer callback and guarantees that
callbacks begin only after `start()` and have synchronously ended when `stop()`
returns. It also exposes a lock-free conservative output-latency estimate; zero
means the backend cannot estimate beyond the current callback period. Supplying
no device keeps the same worker/ring/clock and exposes
`render_callback()` for a host or timer-driven software fallback. Manual calls
are rejected when a physical device owns the callback, and lock-free admission
allows only one ring consumer at a time. Seek and stop close that admission
gate and wait on the control thread for an admitted manual callback before
resetting ring cursors; the realtime callback itself never waits or locks.

`MiniaudioOutputDevice` is compiled when CMake finds the pinned miniaudio
0.11.25 header through `VIDEO_EDITOR_MINIAUDIO_ROOT`, `MINIAUDIO_ROOT`, or the
normal include search. Configure never downloads or vendors it. The source has
a compile-time version assertion. Without that header the same class reports a
typed Unavailable error and the core/fake-device tests remain fully usable.

`MiniaudioDeviceEnumerator` reports connected playback endpoints with stable opaque IDs and a
default marker. `AudioDeviceRecovery` is a non-callback state machine that can stop a disconnected
endpoint and reopen the selected endpoint when a refreshed list reports it again. The desktop
enumerates asynchronously once per second, persists the selected ID, and passes it into realtime
playback startup. Loss pauses the audio master; return retries after serialized stop completes while
the original playback intent remains active. Selecting **System default** stores the empty stable ID.
Native event-driven hot-plug notifications are not used, so detection can lag by one poll interval.

Realtime telemetry publishes callback-safe sample peak/RMS. The callback also copies into a
preallocated, bounded, single-producer queue; `RealtimeLoudnessAnalyzer` consumes it on a dedicated
worker and publishes authoritative EBU-R128 momentary, short-term, and integrated LUFS with version,
validity, staleness, analyzed-frame, and dropped-block fields. Submission never allocates, locks, or
waits. Queue overload marks the reading stale. Offline normalization uses `LoudnessMeter` over
rendered planar blocks and likewise never runs libebur128 in the device callback.

## Diagnostics and current limitations

`PlaybackDiagnostics` reports state, epoch, conservative playback position,
submitted position, estimated latency/uncertainty, queued/pre-rendered frames,
callback count, xrun events, underrun frames, provider exhaustion, device state,
and the latest worker/control error. Counters other than the two position values
are lifetime totals for the playback object. `AsyncPlaybackDiagnostics` adds
requested/effective state, requested/completed/published versions, and the
newest command result/error.

The current worker uses independent provider pulls rather than a persistent
FFmpeg decode lane. The application should therefore use larger decode-ahead
blocks until the sequential decoder exists. Native hot-plug callbacks,
latency calibration/hardware timestamps, time-stretch, and arbitrary buses remain future work.
Accelerated one-hour zero-xrun and two-hour drift simulations cover the bounded core; physical
device/driver/OS matrix endurance remains a release gate.
