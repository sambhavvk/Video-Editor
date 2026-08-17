<!-- SPDX-License-Identifier: MPL-2.0 -->

# Video Editor — Windows source-build preview (not first public beta)

> **Version:** 0.1.0-alpha
>
> **Status:** Engineering preview only. The **first public beta is Linux exclusive**. Signed MSI,
> Windows GPU/D3D11 matrix work, and Windows codec qualification are deferred because packaging
> and GPU compatibility need more calendar time than the Linux beta.
>
> **Last updated:** 2026-08-17

This guide covers the current Windows source build and interactive workflow for contributors. It is
**not** the first public-beta distribution path. Testers who need a first-beta candidate should use
the [Linux guide](ALPHA-TEST-LINUX.md). Keep original media and backups of important projects.

See [Beta feature status](docs/beta-feature-status.md) for exact implementation state and the
[User guide](docs/user-guide.md) for normal workflows.

## System requirements

- Windows 11 x86-64.
- 8 GB RAM minimum; 16 GB recommended.
- At least 15 GB for tools/dependencies/builds, plus media, caches, and exports.
- LLVM `clang-cl` with C++20 support. MSVC's compiler is not supported because exact timeline
  arithmetic uses 128-bit integer intermediates; the Visual Studio linker and Windows SDK are used.
- CMake 3.30+, Ninja, Git, pkg-config support for the dependency prefix, and Python 3.
- Optional D3D11/Vulkan-capable GPU; CPU preview remains available.
- Optional miniaudio 0.11.25 header and usable output device for audible forward-1× playback.

## 1. Install build tools

Install Git for Windows, CMake, Ninja, LLVM, and Visual Studio 2022 Build Tools with the latest
Windows 11 SDK and x64 C++ tools. Open an **x64 Native Tools** PowerShell, then verify:

```powershell
git --version
cmake --version
ninja --version
clang-cl --version
```

## 2. Provide the pinned dependencies

`cmake/DependencyVersions.cmake` is authoritative. The current contracts are:

| Dependency | Required contract |
| --- | --- |
| Qt | 6.11.1 exact; Core, Concurrent, Gui, Widgets, Network |
| FFmpeg | 9.0.1 shared LGPL-compatible build |
| FFmpeg ABI | avformat/avcodec 63.1.101, avutil 61.1.101, swresample 7.1.101, swscale 10.1.101 |
| Protobuf / Abseil | 35.1 / 20250512.1 exact |
| libplacebo | 7.360.1 exact; optional D3D11/Vulkan preview |
| libebur128 | 1.2.6 exact |
| miniaudio | 0.11.25 header; optional physical audio adapter |
| SQLite / OpenSSL | 3.45+ / 3.0+ |
| whisper.cpp | 1.9.2 at `306c88f4d1286aec1bf96e544632897886af5501`; optional local transcription backend |

Use reproducibly built shared dependencies in one prefix. A distribution build must disable FFmpeg
GPL/nonfree configuration and carry the required notices/source offers. FOSS creator delivery needs
`libvpx-vp9` and `libopus`; `vp9_qsv` is optional and always has a libvpx fallback.

Example configuration:

```powershell
$deps = 'C:\video-editor-deps'
$env:CMAKE_PREFIX_PATH = "C:\Qt\6.11.1\msvc2022_64;$deps"
$env:PATH = "C:\Qt\6.11.1\msvc2022_64\bin;$deps\bin;$env:PATH"

cmake --preset windows-dev `
  -DCMAKE_PREFIX_PATH="$env:CMAKE_PREFIX_PATH" `
  -DVIDEO_EDITOR_MINIAUDIO_ROOT="$deps"
```

For local transcription inference, build the exact pinned whisper.cpp library and add:

```powershell
cmake --preset windows-dev `
  -DVIDEO_EDITOR_ENABLE_WHISPER_CPP=ON `
  -DVIDEO_EDITOR_TRANSCRIPTION_WHISPER_INCLUDE_DIR='C:\deps\whisper.cpp\include' `
  -DVIDEO_EDITOR_TRANSCRIPTION_WHISPER_LIBRARY='C:\deps\whisper.cpp\build\src\Release\whisper.lib'
```

Only set `VIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED=ON` when that exact library was built with Vulkan.
This is build provenance, not a claim that one inference used Vulkan. Transcription remains a
truthful unavailable capability when whisper.cpp is omitted.

## 3. Build and verify

```powershell
cmake --build build\windows-dev --parallel $env:NUMBER_OF_PROCESSORS
ctest --test-dir build\windows-dev --output-on-failure
```

The executable is `build\windows-dev\src\app\VideoEditor.exe`. Ensure Qt, FFmpeg, libplacebo,
libebur128, OpenSSL, and optional whisper runtime DLLs are on `PATH` or deployed beside it. Qt also
needs `platforms\qwindows.dll`.

Run the dependency audit before interactive testing:

```powershell
.\build\windows-dev\src\media_codec\video_editor_dependency_audit.exe
```

`expected_abi` must be `true`. A distributable runtime additionally requires
`lgpl_compatible_configuration` to be `true`.

## 4. Run

```powershell
.\build\windows-dev\src\app\VideoEditor.exe
```

Keep PowerShell open to preserve diagnostics.

## Interactive alpha checklist

Record pass, fail, or unavailable for each area.

### 1. Projects and recovery

Create, edit, save, close, and reopen a `.veproj`. With a disposable project, terminate the process
after a completed edit but before checkpoint save, restart, and inspect recovery.

**Expected:** Completed commands survive recovery; saved checkpoints are not corrupted. Media and
caches are referenced, not embedded.

### 2. Import and professional timeline

Import through the file dialog and drag/drop. Test linked A/V insertion, Ctrl/Shift multi-selection,
move, normal/ripple/overwrite trims, roll/slip/slide, linked split/delete, exact frame nudges,
playhead/marker/clip/frame snapping, track create/rename/reorder/lock/visibility/targeting, markers,
gap closure, Escape cancellation, and one-step undo/redo.

**Expected:** Each accepted gesture publishes one exact atomic revision; a rejected or locked batch
does not partially edit the project.

### 3. Titles, transitions, effects, and speed

Author a title; change its text/font/size/alignment/emphasis. Add and edit both transition presets.
Set speed/reverse. Add color/crop/blur effects and Hold/Linear/Bezier keyframes. Save/reopen.

**Expected:** Authoring persists in schema-v3 snapshots and CPU preview/export. Unsupported GPU
frames use per-frame CPU fallback without disabling later compatible GPU frames.

### 4. Preview and professional audio

Play, pause/resume, seek, scrub, and use J/K/L. Select default and named output devices. Test clip
and track gain/pan/fades, mute/solo, EQ, compressor, dialogue reduction, limiter, peak/RMS/LUFS
meters, and loudness normalization. If safe, disconnect/reconnect a nonessential device.

**Expected:** Forward 1× audio is the A/V master when miniaudio opens; otherwise the UI reports
silent fallback. Device loss pauses safely and recovery only resumes intended playback. Reverse and
non-1× audio remain silent. Report every xrun or growing drift.

### 5. Captions and local transcription

1. Import SRT/WebVTT, edit/search cues, and activate a result to seek.
2. Edit alignment, vertical position, safe margin, colors, emphasis, and outline.
3. Export sidecars and styled burn-in; save/reopen and compare.
4. Explicitly download the optional base model. Cancel once during transfer and, if practical, once
   during verification, then complete it. Check that no partial staging directory remains.
5. Transcribe a selected speech clip, cancel once, then complete a new job and activate timed words.
6. Review `Measured silence` and `Transcript filler` items. Toggle selections, apply, undo once, and
   confirm an intervening edit makes an old review stale.

**Expected:** Model bytes are bounded during transfer, verified off the UI thread before atomic
install, and never embedded in the project. Cancellation removes partial staging data.
Inference runs locally in a restartable worker and does not upload media. Measured silence starts
selected; fillers start unselected. Accepted captions and cuts form one atomic revision. Cancel,
failure, discard, or stale review leaves project state unchanged. A build without whisper.cpp must
show the backend as unavailable without disabling manual captions.

### 6. Proxies and creator export

Create/cancel/recreate a recommended proxy, preview it, then export from originals. Test FFV1/MKV,
ProRes/MOV when available, YouTube/vertical VP9+Opus WebM, Opus podcast, custom rate/resolution/
bitrate, captions, cancellation, and existing-destination protection.

**Expected:** Partial proxy/export files are not committed. A failed QSV attempt visibly restarts
the complete export with `libvpx-vp9`. H.264/AAC are intentionally unavailable.

### 7. Stability and accessibility

Edit continuously for at least 20 minutes. Resize/dock panels, use keyboard focus and accessible
names, monitor Task Manager, and inspect terminal output.

**Expected:** No crash, freeze, unbounded memory, corrupt project, stuck export, or worker left after
exit.

## Known alpha limitations

- Physical one-hour zero-xrun and two-hour A/V drift qualification is incomplete across supported
  Windows audio devices; hot-plug uses one-second polling.
- GPU preview is offscreen and downloaded into Qt; native swapchain presentation, zero-copy decode,
  full GPU effect/color parity, and HDR mastering are not implemented.
- H.264/AAC remains disabled pending legal approval; creator delivery uses FOSS VP9/Opus.
- Local transcription requires the optional pinned backend and an on-demand approximately 141 MiB
  model. Physical multilingual accuracy, Vulkan inference, and worker-death matrices remain open.
- Caption burn-in uses deterministic bitmap glyphs, not production font shaping; embedded subtitle
  streams are not implemented.
- Relinking, persistent proxy discovery, media reconstruction, desktop thumbnails/
  waveforms/metadata, and the cache browser are implemented in this slice. Physical
  unplug/disk-full matrices, packaging/signing, and the 200+ media corpus remain.

## Reporting issues

Include summary, exact steps, expected/actual result, reproducibility, `git rev-parse HEAD`, terminal
output, and:

```powershell
Get-ComputerInfo | Select-Object WindowsProductName, WindowsVersion, OsBuildNumber, CsTotalPhysicalMemory
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
cmake --version
clang-cl --version
git rev-parse HEAD
```

For transcription include whether whisper/Vulkan were enabled, model state, selected clip format,
worker error, and whether project state changed. For playback include output device and GPU/CPU
status. For export include preset, actual encoder/fallback, caption mode, and destination safety.

This alpha is intended to find correctness, reliability, and usability defects. Windows findings do
not gate the Linux-first public beta. Signed MSI, Windows GPU matrix, and Windows codec
qualification remain deferred.
