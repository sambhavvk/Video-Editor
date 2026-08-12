<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0009: Originals-only 48 kHz timeline audio render and mux

- **Status:** accepted
- **Date:** 2026-08-08
- **Owners:** Core/Media, Desktop/Product, and Quality/Platform
- **Supersedes in part:** ADR 0004's intentionally video-only export restriction

## Context

ADR 0004 established a safe video-only reference exporter while no connected timeline audio
renderer existed. The edit model already stores audio clips, gain, pan, fades, mute/solo, rate, and
reverse state. Public beta requires those exact immutable edit decisions to produce a deterministic
48 kHz stereo master and to be muxed without weakening revision binding, original-media authority,
sample-count accuracy, cancellation, or destination safety.

The realtime device callback and the offline decoder/export worker have different safety rules. An
FFmpeg decoder performs allocation, locking, seeking, resampling, and filesystem I/O and therefore
cannot execute in the callback.

## Decision

- `audio_render` accepts one immutable `TimelineSnapshot` and an exact absolute half-open sample
  request. Its master format is 48 kHz, two-channel, planar float32. A successful result contains
  precisely the requested samples and carries no mutable decoder state across revisions.
- An originals-only provider resolves asset IDs to source paths and stream indexes. Its public type
  cannot represent a proxy, preventing proxy audio from becoming preview-independent export
  authority.
- Source mapping uses exact rational edit time. The renderer handles gaps as zero, sums overlapping
  clips without an implicit limiter, respects track mute and global solo selection, and applies
  clip gain, constant-power pan, linear fades, playback rate, and reverse mapping.
- FFmpeg decode and resample work runs on a decode/export thread. Cancellation is checked through an
  interrupt callback and between packets, frames, clips, and mix work. Typed errors identify the
  relevant asset and clip where possible.
- Export video and audio are compiled from the same frozen sequence revision. The exact audio count
  is `ceil(sequence_duration × 48000)`. Audio PTS derives from absolute sample position in a 1/48000
  time base; the export loop requests consecutive blocks covering that half-open master range and
  records exact encoded sample counts.
- The export service packs deterministic signed 16-bit little-endian stereo PCM in both reference
  containers. Bounded 960-sample (20 ms) packets preserve exact Matroska millisecond starts while
  limiting memory. Quantization must not change timeline sample count or use a proxy. Creator
  H.264/AAC remains a separate legal, packaging, and codec-matrix gate.
- Audio packets join the same unique temporary sibling as video. Cancellation or any render,
  encode, mux, flush, or pre-commit failure removes the temporary result and leaves the destination
  unchanged. Only a fully flushed trailer is atomically committed.
- The audio-device adapter consumes pre-rendered blocks from a bounded lock-free ring and uses a
  latency-compensated playback position as the master clock. It never calls the offline renderer
  directly; the provider maps the immutable snapshot into exact requests on a pre-render worker.

## Consequences

- A deterministic offline audio reference can be verified independently of device timing and later
  reused by export workers.
- Clip audio Inspector changes gain real render semantics without claiming that mixer effects,
  dialogue processing, normalization, limiting, or release-grade realtime playback are finished.
- Audio and video export share revision, cancellation, and atomic destination guarantees, while
  keeping codec policy outside the edit model.
- Repeated self-contained block requests favor determinism over decode reuse. Decode-ahead/session
  optimization may be added behind the same sample-range contract after correctness tests.

## Verification

Module tests cover exact/nonzero ranges, source offsets, nonzero input PTS, gaps and overlap sums,
rate/reverse mapping, gain/pan/fades, mute/solo, missing originals, cancellation, and bit-identical
repeated requests. Export tests must additionally verify exact requested/encoded sample counts,
monotonic 1/48000 audio PTS, container decode-back, A/V duration bounds, cancellation at audio
phases, repeated deterministic output where the codec permits it, and preservation of an existing
destination.

Realtime device-clock, xrun, and two-hour A/V drift tests remain public-beta gates rather than
claims of this offline contract.
