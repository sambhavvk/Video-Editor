# X03 — Pitch-preserving retime (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X03). Discovery only; no implementation claimed.

## Options considered

| Option | Status | Why not first |
| --- | --- | --- |
| Voiceover recording | Missing | Needs device capture path + new track semantics |
| Dialogue/music buses | Deferred (`beta-feature-status.md`) | Surround/bus graph out of beta |
| Sends / automation | Partial track DSP only | Large mixer model |
| Pitch-preserving retime | Speed 0.01×–100× exists; pitch change at ≠1× | **Selected** — extends shipped speed UI |
| Speed-ramp curves | Keyframed rate not in model | Depends on retime engine first |

## Chosen workflow: pitch-preserving clip retime (first slice)

**Bounded slice:** Per-clip **constant** rate with a **Preserve pitch** checkbox on existing speed
controls. When enabled and rate ≠ 1×, audio is time-stretched; video frame selection stays on the
existing timeline rate model (duplicate/drop frames). Export and offline audio render use the same
stretch as realtime preview.

### FOSS path

Official FFmpeg in [B02](foss-component-register.md) / `src/media_codec/CMakeLists.txt` pins
**libavformat, libavcodec, libavutil, libswresample, libswscale** only. The tree does **not** link
`libavfilter`. Clip speed today resamples on the existing `swresample` path (pitch changes). There
is no `atempo` (or any avfilter) usage.

| Component | Role | Eligibility |
| --- | --- | --- |
| [SoundTouch](https://www.surina.net/soundtouch/) 2.3.x | Primary pitch-preserving stretch for preview blocks + offline render | **LGPL-2.1** — **not selected** until a B02 pin; dynamic link like libebur128 |
| Current `swresample` rate path | Fallback when SoundTouch is off: same pitched resample as today | Already selected |
| FFmpeg `libavfilter` / `atempo` | Not in the official library set | **Not selected**; adding it needs a B02 + CMake pin, not a silent extra link |
| Rubber Band Library | Higher quality | **GPL** — **rejected** for official builds |

Prototype plan: optional SoundTouch behind `audio_time_stretch` in `timeline_audio_render` (same
optional-capability pattern as whisper.cpp). If SoundTouch is unavailable, disable **Preserve pitch**
and keep today's resample. Do not assume `atempo` is already in the LGPL official build.

### Latency and state expectations

| Path | Expectation |
| --- | --- |
| Realtime playback | Stretch runs on pre-render worker blocks (not audio callback). One block latency increase acceptable; document max rate 4× for preserve-pitch realtime v1 |
| Scrub / shuttle JKL | Unchanged: transport shuttle stays resampled, not pitch-preserved (`user-guide.md`) |
| Export | Offline render only; deterministic stretch parameters logged in export metadata |
| Undo | `SetClipPreservePitchCommand` + existing `SetClipSpeedCommand` batching |

### Preview / export checks

1. 2× speed interview clip: waveform duration halves, fundamental pitch within ±50 cents of 1× (ear test + automated spectral centroid tolerance on lavfi tone fixture).
2. Reverse + preserve pitch: rejected v1 (checkbox disabled when reverse enabled).
3. Export Opus/VP9 creator preset: A/V duration agreement within one video frame.
4. GPU preview: unchanged video path; audio stretch on CPU worker.

### Remaining choices deferred

- Speed-ramp curves, per-keyframe rate, rubber-band quality tier.
- Voiceover record, buses, surround, automation lanes.

## Non-goals (leave for later)

- Full DAW, surround pipeline, or opaque "AI enhance" bundles.
- Pitch-preserving **video** optical flow (frame interpolation).
- GPL Rubber Band in official packages.
- Linking `libavfilter` / `atempo` without a B02 selection.
- Pitch-preserving **JKL shuttle** (clip-rate only in this slice).

## Follow-up chunks

| ID | Work |
| --- | --- |
| X03a | B02 register SoundTouch pin + CMake optional module |
| X03b | `preserve_pitch` clip field (schema), stretch in audio render |
| X03c | Inspector checkbox, preview/export tests, disable when reverse |
