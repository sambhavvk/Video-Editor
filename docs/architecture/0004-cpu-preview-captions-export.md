<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0004: CPU preview, caption, and reference-export contracts

- Status: Accepted; video-only export decision superseded in part by ADR 0009
- Date: 2026-08-06
- Owners: Core/Media and Desktop/Product

## Context

The first integrated desktop slice needs to exercise the same immutable timeline used by later GPU
preview and worker export. It must also avoid presenting unfinished audio or codec paths as if they
were release-ready.

> **Historical note:** This ADR records why the initial reference exporter was deliberately
> video-only. [ADR 0009](0009-timeline-audio-render-and-mux.md) adds the originals-only 48 kHz
> timeline-audio and mux contract; the remaining preview, caption, revision, proxy, and destination
> safety decisions below remain in force.

## Decision

- `playback` implements the render engine's narrow `FrameProvider` interface. It owns persistent
  FFmpeg sessions, exact source-time seeking, original/proxy selection, and request-epoch
  cancellation. The edit model and UI do not receive FFmpeg types.
- `caption_service` owns deterministic SRT/WebVTT parsing, validation, serialization, reflow, and
  transcript search. It converts to ordinary `edit::Caption` entities; imports are one coalesced,
  journaled edit gesture.
- `export_service` compiles immutable snapshots through `CpuRenderer` at full quality with proxies
  disabled. Its initial safe formats are FFV1/Matroska and encoder-gated ProRes/MOV. Output is
  written to a temporary sibling and atomically committed.
- The initial exporter is explicitly video-only. Requests for audio fail with a typed error, and
  the desktop UI labels this limitation. It must not imply that a silent file contains a finished
  48 kHz stereo mix.
- Preview and export use independent FFmpeg providers so their cancellation epochs and decoder
  state cannot invalidate one another.

## Consequences

The working slice can verify exact edit-to-frame behavior, caption round-trips, cancellation, and
destination safety before GPU and realtime-audio complexity is introduced. Creator delivery
At the time of this decision, presets requiring muxed audio or H.264 remained release-blocked until
the audio renderer, licensed encoder bundle, and worker integration passed their gates. ADR 0009
subsequently resolves the deterministic PCM reference-master part, not the H.264/AAC creator gate.
