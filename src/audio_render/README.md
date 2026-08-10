<!-- SPDX-License-Identifier: MPL-2.0 -->

# Timeline audio renderer

`video_editor::audio_render` is the offline/decode-thread bridge from an immutable
`edit::TimelineSnapshot` to the beta master format: exact absolute sample ranges
of 48 kHz, stereo, planar `float32` audio.

## Contract

- `OriginalAudioProvider` resolves an asset ID to one authoritative original
  path and audio-stream index. The contract intentionally cannot represent a
  proxy, so export code cannot accidentally render proxy audio.
- `OriginalAudioRegistry` is a thread-safe concrete provider. Registering media
  normalizes paths but does not touch the filesystem.
- `TimelineAudioRenderer::render` accepts a frozen snapshot, signed absolute
  `start_sample`, exact `sample_count`, and optional `std::stop_token`. It returns
  either an `audio::AudioBlock` with precisely that range or a typed,
  asset/clip-attributed error.
- Rendering performs allocations, filesystem I/O, demux, decode, and resampling.
  It belongs on a decode/export worker and is forbidden in a device callback.
- Timeline intervals are half-open. The first output sample is evaluated at
  `start_sample / 48000`; source positions use exact rational edit-model math
  and floor rounding. Reverse playback maps the first sample to the final source
  sample in the half-open source range.
- Audio tracks obey mute and global solo selection. Active clips are summed
  without implicit limiting. Clip gain is in dB, pan is constant-power, and
  linear fades are evaluated in timeline time. Gaps are deterministic zeroes.

The decoder installs an FFmpeg interrupt callback and also checks cancellation
between packets, frames, clips, and mixed samples. It seeks with one second of
preroll when stream timestamps permit and only retains source samples requested
by the current timeline block. Every request owns its decoder state, avoiding
revision or resampler-history leakage between deterministic export requests.

## Current limitations

- Playback-rate conversion is deterministic nearest-left sample selection; a
  later high-quality time-stretch/resampling policy can replace it without
  changing timeline time or sample-count semantics. Pitch is not preserved.
- Track/clip effect chains, dialogue DSP, loudness normalization, and master
  limiting are separate graph stages and are not applied here yet.
- This renderer produces stereo blocks but does not drive an audio device or
  mux encoded audio. Export integration should request consecutive blocks from
  the same immutable revision, encode them, and use the block's absolute sample
  position as authoritative PTS.
- Corrupt streams and timestamp discontinuities return typed errors or silence
  where the original contains a real gap; concealment policy remains future
  work.

## Verification

The module tests generate deterministic PCM fixtures and cover exact ranges,
timeline/source offsets, rate and reverse mapping, overlap summation, gain/pan,
fades, mute/solo, nonzero input PTS, missing originals, cancellation, and
bit-for-bit repeated requests. The target participates in the repository's
strict-warning and ASan/UBSan options once added after `audio_engine` in the
media dependency order.
