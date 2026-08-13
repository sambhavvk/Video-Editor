<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0016: FOSS creator delivery with VP9 and Opus

- **Status:** accepted
- **Date:** 2026-08-13
- **Owners:** Core/Media and Quality/Platform

## Context

The reference exporter produces deterministic FFV1/Matroska and optional ProRes/MOV masters, but
those files are not the normal upload artifacts expected by online creators. H.264/AAC remains
outside the approved distribution configuration pending specialist legal and binary-redistribution
review. The editor still needs a creator-delivery path that can be built and tested without enabling
those codecs.

## Decision

### Delivery codec and availability

- Creator platform presets use VP9 video and Opus audio in WebM. The software implementation
  requires the FFmpeg `libvpx-vp9` and `libopus` encoders and fails closed when either encoder needed
  by a selected preset is unavailable.
- YouTube and vertical presets produce video plus optional audio. The podcast preset produces an
  Opus-only WebM with no synthetic video stream.
- H.264/AAC capability reporting and legal build gates remain present, disabled, and unrelated to
  the FOSS delivery preset. This ADR is not a legal conclusion about any codec or distribution.

### Creator controls

- A platform preset supplies default dimensions, video bitrate, audio bitrate, and optional frame
  rate. Explicit request values override those defaults.
- VP9 accepts either a validated video bitrate or an explicit constant-quality value from 0 through
  63. The result records the actual video and audio encoder names rather than inferring them from
  the selected preset.
- Resolution conversion preserves the source display aspect ratio and letterboxes the remaining
  area. VP9 4:2:0 output dimensions are even. Frame-rate conversion samples the same immutable
  timeline at exact rational output-frame times and exports `ceil(duration × output rate)` frames.
- Audio is rendered from authoritative originals in exact 48 kHz timeline ranges. Opus codec delay
  is container metadata; decode-back verification checks the intended timeline sample span.
- Caption burn-in uses the CPU reference frame before scaling and encoding. SRT/WebVTT sidecars
  remain atomic companion outputs. Burn-in is invalid for an audio-only podcast.

### Safety and fallback

- Media is written to a unique sibling temporary file, flushed, and atomically committed. A
  cancelled or failed encode cannot replace the destination with a partial file.
- The Qt panel performs only lightweight encoder-presence checks. Windows may select FFmpeg
  `vp9_qsv`; Linux may select `vp9_vaapi`, but actual device creation is deferred to the export
  worker so graphics-driver calls cannot delay UI construction or shutdown. Software encoder
  availability is established separately, so a missing or unusable hardware endpoint never
  disables creator presets that libvpx can serve.
- The hardware path owns an `AVHWDeviceContext`, an NV12 `AVHWFramesContext`, exact Rec.709
  CPU-to-NV12 conversion, upload, and frame submission. Result metadata records the actual encoder
  and whether hardware completed the export.
- Hardware setup, frame-pool, upload, device, or encoder failure is typed separately from source
  render, caption, audio, mux, progress-callback, and cancellation failures. Only the typed hardware
  failure discards its unique temporary file and restarts the complete immutable export once with
  deterministic `libvpx-vp9`. Cancellation and ordinary failures never retry; neither attempt can
  replace the destination before its own successful atomic commit. Progress emits an explicit
  restart event so the desktop resets its bar and explains the software fallback.

## Consequences

Creators can produce upload-ready WebM files without enabling H.264/AAC. Reference masters retain
their existing codec and determinism contracts. Hardware VP9 is an optional acceleration of the
same immutable render; it never becomes authoritative and never removes the software path. The
supported Windows/Linux hardware matrix remains a release-validation obligation.

## Required verification

- Lightweight software/hardware-encoder presence for `libvpx-vp9`, `libopus`, QSV VP9, and VAAPI
  VP9, with actual device validation isolated to the export worker.
- Canary-guarded exact NV12 luma/chroma conversion plus hardware device/frame/upload/encode tests.
- Deterministic failure injection proving one software retry only for hardware causes, no retry for
  cancellation or ordinary render/audio/mux failure, fallback progress reset, and preservation of
  an existing destination across failed attempts.
- Decode-back codec, stream topology, dimensions, frame timestamps/counts, audio sample span, and
  Rec.709 metadata for representative creator presets.
- Podcast output with one Opus stream and no video stream.
- Invalid rate, dimension, bitrate, quality, audio-only burn-in, and unavailable-encoder failures.
- Cancellation and existing-destination preservation for creator output and caption sidecars.
