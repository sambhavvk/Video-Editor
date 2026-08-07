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
- FFmpeg 8.1 media probing, keyframe-accurate seek/decoding, and a deterministic CPU render path.
- Qt 6 Widgets desktop shell with Import, Edit, Audio & Captions, and Deliver workspaces.
- Restartable job protocol for proxy, export, analysis, and transcription workers.

## Usable vertical slice

The desktop application currently supports drag-and-drop or dialog import, a searchable media bin,
linked video/audio insertion, split and ripple-delete commands, JKL transport, exact undo/redo,
asynchronous software preview, atomic save/reopen, SRT/WebVTT import/export and transcript search.
Its reference exporter produces deterministic, originals-only FFV1/Matroska or ProRes/MOV video
masters without damaging an existing destination when it fails or is cancelled.

Run it after building:

```sh
./build/dev/src/app/VideoEditor [media files...]
```

For a headless UI smoke capture:

```sh
QT_QPA_PLATFORM=offscreen ./build/dev/src/app/VideoEditor --screenshot editor.png
```

Current vertical-slice limitations are deliberately visible in the UI: the CPU exporter is
video-only; realtime audio-device playback, muxed audio export, automatic proxy generation, GPU
presentation, and local Whisper model execution are not yet integrated. The lower-level audio DSP,
proxy policy, worker protocol, caption, playback, and render contracts are present and tested for
the next quality gates.

## Build

The development environment requires CMake 3.30+, Ninja, a C++20 compiler, Qt 6.11, SQLite 3,
FFmpeg 8.1 libraries, libplacebo, Protobuf, and GTest.

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

See [CONTRIBUTING.md](CONTRIBUTING.md) and the architecture decisions under
[`docs/architecture`](docs/architecture).

## License

Source code in this repository is licensed under the Mozilla Public License 2.0. Third-party
libraries, optional codec components, and model files retain their own licenses. Distribution
builds must pass the dependency and codec licensing gates documented in this repository.
