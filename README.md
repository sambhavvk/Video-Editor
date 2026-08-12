<!-- SPDX-License-Identifier: MPL-2.0 -->

# Video Editor

An offline-first, non-destructive desktop video editor for Windows and Linux. The project is
designed for a beginner-friendly creator workflow without imposing a separate simplified editing
model that users later have to abandon.

> **Status:** working CPU-first vertical slice, not a public beta. Project formats and internal
> APIs are pre-beta and may still evolve through explicit migrations.

## Current architecture

- Dependency-free C++20 edit model with exact rational time and revisioned commands.
- Transactional SQLite `.veproj` project store, deterministic protobuf snapshots, and recovery
  journal.
- FFmpeg probing/decoding under the centrally pinned contract, an originals-only 48 kHz timeline
  audio renderer, and a deterministic CPU video compositor.
- Optional miniaudio 0.11.25 realtime 48 kHz stereo output with a bounded pre-render ring,
  latency-compensated audio-master playhead, and nonblocking versioned GUI controls; a
  capability-gated libplacebo D3D11 (Windows)/Vulkan (Linux) preview path with CPU fallback.
- Qt 6 Widgets desktop shell with Import, Edit, Audio & Captions, and Deliver workspaces.
- Restartable job protocol for proxy, export, analysis, and transcription workers.

## Usable vertical slice

The desktop application currently supports drag-and-drop or dialog import, a searchable media bin,
linked video/audio insertion, move/trim/split/ripple and precision edit commands, JKL transport,
exact undo/redo, asynchronous software preview, atomic save/reopen, user-triggered proxy creation,
and editable/searchable SRT/WebVTT captions. Canonical titles and transitions are revisioned,
schema-v2 project state with deterministic CPU rendering, although their desktop authoring controls
are not connected yet. The Inspector writes revisioned transform, crop,
opacity, blend, clip-gain, pan, and fade properties into the same model used by rendering and
export. Audio-track mute/solo is revisioned and consumed by the offline mix.

Its reference exporter produces deterministic, originals-only FFV1/Matroska or ProRes/MOV masters
with exact video-frame and 48 kHz stereo PCM sample counts. Failure or cancellation leaves an
existing destination unchanged.

Run it after building:

```sh
./build/dev/src/app/VideoEditor [media files...]
```

For a headless UI smoke capture:

```sh
QT_QPA_PLATFORM=offscreen ./build/dev/src/app/VideoEditor --screenshot editor.png
```

Current vertical-slice limitations are deliberately visible in the UI: device selection/hot-plug,
meters, track DSP/gain, reverse/non-1× audible transport, automatic proxy scheduling, native GPU
presentation and zero-copy, local Whisper execution, title/transition authoring, keyframe editing,
creator codecs/presets, and the broader effects and color toolset are not yet integrated. Packaging
and the representative media corpus also remain release blockers.

## Build

The development environment requires CMake 3.30+, Ninja, a C++20 compiler, Qt 6.11, SQLite 3,
the centrally pinned FFmpeg libraries, libplacebo, Protobuf, and GTest. See the dependency guide
for the active ABI contract.

Windows development builds currently use LLVM `clang-cl` (the `windows-dev` preset) so the exact
timeline's 128-bit intermediates are available with the MSVC ABI. Native MSVC is rejected at
configure time instead of producing an unreliable or late compile failure.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

For work on the edit model and project store without desktop/media dependencies:

```sh
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

Start with the [documentation index](docs/index.md), then see [CONTRIBUTING.md](CONTRIBUTING.md)
and the decisions under [`docs/architecture`](docs/architecture).

## License

Source code in this repository is licensed under the Mozilla Public License 2.0. Third-party
libraries, optional codec components, and model files retain their own licenses. Distribution
builds must pass the dependency and codec licensing gates documented in this repository.
