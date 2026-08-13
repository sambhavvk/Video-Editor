<!-- SPDX-License-Identifier: MPL-2.0 -->

# Export service

`VideoEditor::ExportService` is the revision-bound CPU export path. It consumes one immutable
`TimelineSnapshot`, a `CpuRenderer`, and, when requested, an
`audio_render::TimelineAudioRenderer`. Video uses original media, full-quality effects, and no
proxies. The audio provider contract can resolve only authoritative originals.

## Output families

Reference masters remain available as lossless 10-bit FFV1/Matroska and, when its encoder exists,
10-bit ProRes 422 HQ/MOV. They carry deterministic 48 kHz stereo PCM S16LE audio.

Creator presets always have a FOSS software path:

- YouTube 1080p, 1440p, and 2160p: `libvpx-vp9` video plus optional `libopus` audio in WebM;
- vertical 1080×1920 and 720×1280: VP9 plus optional Opus in WebM;
- podcast audio-only: one Opus stream in WebM and no synthetic video stream.

The creator request may override even output dimensions, exact rational frame rate, video bitrate,
VP9 quality (0–63), audio bitrate, and caption mode. Scaling preserves display aspect ratio and
letterboxes the unused area. Frame-rate conversion samples the immutable timeline at exact output
frame times and emits `ceil(duration × output rate)` frames.

Caption modes are none, CPU burn-in, SRT/WebVTT sidecar, or burn-in plus sidecar. Audio-only output
rejects burn-in. Sidecar files use the caption service's nearest-millisecond serializer and their
own atomic write.

## Exactness and safety

Reference frames are transformed to limited-range Rec.709 through the deterministic packing path.
Audio is pulled in exact half-open ranges from sample zero through
`ceil(sequence_duration × 48000)`. Results record actual encoder names, frame count, audio sample
count, duration, hardware-use status, and any sidecar path.

Every media encode targets a unique sibling temporary file. Cancellation and pre-commit failures
remove it; a completed file is flushed and atomically committed, so a partial encode cannot replace
an existing destination.

## Capability boundary

Creator delivery requires `libvpx-vp9`; output with audio also requires `libopus`. Missing encoders
produce a typed unavailable result and the Deliver panel disables incompatible presets. H.264/AAC
gates remain off and are not used by the FOSS path.

The Deliver UI performs only lightweight encoder-presence checks. When requested, the export
worker selects `vp9_qsv` on Windows or `vp9_vaapi` on Linux and validates the real device there, so
graphics-driver calls cannot delay panel construction or shutdown. A usable hardware path creates
an `AVHWDeviceContext` and NV12 `AVHWFramesContext`, uploads the deterministic Rec.709 frame, and
reports the actual encoder.
Hardware setup, pixel-format, frame-pool, upload, device, and encode failures have a dedicated error
cause. That cause alone discards the temporary attempt and restarts once from frame zero with
`libvpx-vp9`; source render, caption, audio, mux, callback, cancellation, and software failures do
not retry. The restart is an explicit progress event. Neither attempt commits before completion.

Tests decode outputs and cover stream topology, exact frame/sample spans and timestamps,
dimensions, frame-rate conversion, codec metadata, podcast audio-only output, caption modes,
validation, unavailable encoders, exact/canary-guarded NV12 conversion, typed failure retry rules,
fallback progress, cancellation, and destination preservation. Hardware completion remains an
environment-gated smoke test; deterministic failure injection covers fallback on CPU-only CI.
