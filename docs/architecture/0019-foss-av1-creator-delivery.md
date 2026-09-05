<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0019: FOSS AV1 creator delivery and embedded WebVTT

- **Status:** accepted
- **Date:** 2026-09-05
- **Owners:** Core/Media and Quality/Platform

## Context

ADR 0016 established VP9/Opus WebM as the FOSS creator-delivery path. Upload platforms
increasingly accept AV1 in WebM, and many FFmpeg builds ship `libsvtav1` or `libaom-av1` alongside
the existing VP9 stack. Creators also need captions muxed back into exported media, not only as
burn-in pixels or sidecar files.

H.264/AAC remain outside the approved distribution configuration.

## Decision

### AV1 creator codec selector

- Add `VideoPreset::Av1OpusWebm` beside the existing `Vp9OpusWebm` delivery preset.
- Creator YouTube and vertical platform presets accept a `CreatorVideoCodec` selector (`vp9` or
  `av1`). Podcast audio-only delivery ignores the selector and remains Opus-only WebM without a
  synthetic video stream. Reference FFV1/ProRes presets ignore the selector.
- `creator_av1_available()` is true when (`libsvtav1` OR `libaom-av1`) AND Opus encoders are
  present. `platform_preset_available()` continues to gate VP9+Opus only.
- Software AV1 encoding prefers `libsvtav1` (`preset=8`, CRF/qp or bitrate), then `libaom-av1`
  (`cpu-used=6`, `row-mt=1`, CRF or bitrate). VP9-only libvpx options are not applied to AV1.
- Linux may select `av1_vaapi` when device creation succeeds in the export worker. Hardware setup,
  upload, or encode failure discards the attempt and restarts the complete atomic export once with
  software AV1, matching the VP9 hardware contract in ADR 0016.

### Embedded WebVTT captions

- `CaptionExportMode` adds `Embedded` and `BurnInAndEmbedded`. Helpers
  `caption_mode_burns_in`, `caption_mode_writes_sidecar`, and `caption_mode_embeds` classify
  modes for export validation.
- After video and audio streams are configured, WebM and Matroska exports may add a `webvtt`
  subtitle stream. Cues use UTF-8 text with PTS and duration derived from canonical caption ranges
  and the video time base (or 1/1000 for audio-only rejection). Embedded captions are rejected for
  podcast audio-only delivery and unsupported containers.
- Sidecar SRT/WebVTT output remains an atomic companion write after media commit, unchanged in
  failure semantics.

### Job and UI contracts

- `ExportOptions.video_codec` (`""`|`vp9`|`av1`, default VP9) and extended `caption_mode` strings
  (`embedded`, `burn_in_and_embedded`) are parsed fail-closed by the export worker.
- The Deliver panel exposes a creator video-codec combo and relabeled quality/hardware controls;
  AV1 is disabled when `creator_av1_available()` is false or the podcast preset is selected.

## Consequences

Creators can choose VP9 or AV1 FOSS WebM delivery without enabling H.264/AAC. Embedded WebVTT
closes the export loop opened by text-subtitle extraction on import. Hardware AV1 remains optional
acceleration with the same single software retry and atomic commit guarantees as VP9.

## Required verification

- `creator_av1_available`, `creator_video_preset_for`, and caption-mode helper unit tests.
- Decode-back AV1+Opus WebM export when an AV1 encoder is present; skip otherwise.
- Embedded WebVTT round-trip via `list_subtitle_streams` and `extract_text_subtitles`.
- Hardware-failure injection still exercises one software retry for creator WebM delivery.
- Deliver panel preset count remains eight with AV1 selector and embedded caption modes exposed.

## Related

- [ADR 0016: FOSS creator delivery with VP9 and Opus](0016-foss-creator-delivery.md)
