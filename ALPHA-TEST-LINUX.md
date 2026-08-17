<!-- SPDX-License-Identifier: MPL-2.0 -->

# Video Editor — Linux test guide (first public beta target)

> **Version:** 0.1.0-alpha
>
> **Status:** Linux engineering preview toward a Linux-exclusive first public beta; not for
> irreplaceable work. Windows is out of first-beta scope.
>
> **Last updated:** 2026-08-17

Thank you for testing. This guide covers a source build and the current Linux workflow toward the
**first public beta, which is Linux exclusive**. The editor is offline-first and does not require an
account. Keep original media and backups of important projects. Windows MSI signing and the Windows
GPU/codec matrix are deferred; do not treat a Linux test pass as Windows qualification.

For precise implementation status, see [Beta feature status](docs/beta-feature-status.md). For
normal application usage, see the [User guide](docs/user-guide.md).

---

## System requirements

- **Validated target:** Ubuntu 24.04/26.04 or the current Fedora release, x86-64. Other current
  glibc-based distributions are best-effort for source builds.
- **RAM:** 8 GB minimum; 16 GB recommended.
- **Disk:** At least 15 GB for dependencies and build trees, plus media, proxies, and exports.
- **Compiler:** A C++20 compiler with `__int128`; current GCC or Clang is recommended.
- **Build tools:** CMake 3.30+, Ninja, pkg-config, Git, and Python 3.
- **Display:** Wayland or X11 through Qt.
- **GPU:** Optional Vulkan 1.2-capable Intel, AMD, or NVIDIA GPU. CPU preview remains available when
  libplacebo, Vulkan, or a usable device is unavailable.
- **Audio:** Optional output device supported by miniaudio. A build without the pinned miniaudio
  header runs video silently and reports that fallback explicitly.

## 1. Install system build tools

These packages provide the compiler, CMake tooling, Qt platform prerequisites, and Vulkan headers.
They do not replace the exact project dependencies listed in the next section.

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git python3 \
  libgl-dev libegl-dev libxkbcommon-dev \
  libvulkan-dev mesa-vulkan-drivers vulkan-tools
```

### Fedora

```bash
sudo dnf install -y \
  gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git python3 \
  mesa-libGL-devel mesa-libEGL-devel libxkbcommon-devel \
  vulkan-loader-devel mesa-vulkan-drivers vulkan-tools
```

### Arch Linux — best effort

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf git python \
  mesa libxkbcommon vulkan-icd-loader vulkan-tools
```

After installation, run `cmake --version`. If it is older than 3.30, install a newer CMake before
configuring the project; the repository intentionally rejects older versions.

## 2. Provide the pinned dependencies

The authoritative versions are in `cmake/DependencyVersions.cmake`. At this revision they are:

| Dependency | Required contract |
| --- | --- |
| Qt | 6.11.1 exact; Core, Concurrent, Gui, Widgets, and Network |
| FFmpeg | 9.0.1 exact; shared LGPL-compatible build |
| FFmpeg ABI | avformat/avcodec 63.1.101, avutil 61.1.101, swresample 7.1.101, swscale 10.1.101 |
| Protobuf | 35.1 exact |
| Abseil | 20250512.1 exact |
| libplacebo | 7.360.1 exact; optional GPU acceleration |
| libebur128 | 1.2.6 exact |
| miniaudio | 0.11.25 header; optional but required for physical audio playback |
| whisper.cpp | 1.9.2 at `306c88f4d1286aec1bf96e544632897886af5501`; optional local transcription backend |
| SQLite | 3.45 or newer |
| OpenSSL | 3.0 or newer |
| GTest | Required when `VIDEO_EDITOR_BUILD_TESTS=ON` |

The repository does not yet ship a complete redistributable dependency bundle. Use locally built
shared libraries or another reproducible prefix that satisfies these exact contracts. Arbitrary
distribution or vcpkg versions may configure incorrectly or fail the runtime ABI audit.

The FFmpeg build used for alpha validation should dynamically link compatible libraries and must
not enable GPL or nonfree configuration for distribution. FOSS creator delivery additionally needs
the `libvpx-vp9` and `libopus` encoders. `vp9_vaapi` is optional; failed hardware setup or encoding
automatically restarts that export with `libvpx-vp9`.

Example configuration with a local dependency prefix:

```bash
export VIDEO_EDITOR_DEPS=/absolute/path/to/video-editor-deps
export CMAKE_PREFIX_PATH="$VIDEO_EDITOR_DEPS"
export PKG_CONFIG_PATH="$VIDEO_EDITOR_DEPS/lib/pkgconfig:$VIDEO_EDITOR_DEPS/lib64/pkgconfig"
export LD_LIBRARY_PATH="$VIDEO_EDITOR_DEPS/lib:$VIDEO_EDITOR_DEPS/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cmake --preset dev \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" \
  -DVIDEO_EDITOR_MINIAUDIO_ROOT="$VIDEO_EDITOR_DEPS"
```

If Qt is installed separately, include both prefixes:

```bash
cmake --preset dev \
  -DCMAKE_PREFIX_PATH="/absolute/path/to/Qt/6.11.1/gcc_64;$VIDEO_EDITOR_DEPS" \
  -DVIDEO_EDITOR_MINIAUDIO_ROOT="$VIDEO_EDITOR_DEPS"
```

During configuration, check the realtime-audio message:

- `Realtime audio: miniaudio 0.11.25 adapter enabled` means physical playback can be tested.
- `miniaudio adapter unavailable; manual callback fallback enabled` means preview will be silent.

## 3. Build

Use the developer preset for alpha testing:

```bash
cmake --preset dev
cmake --build --preset dev -j"$(nproc)"
```

The application is written to:

```text
build/dev/src/app/VideoEditor
```

For an optimized build:

```bash
cmake --preset release
cmake --build --preset release -j"$(nproc)"
```

Useful configuration switches:

| CMake variable | Default | Purpose |
| --- | --- | --- |
| `VIDEO_EDITOR_BUILD_TESTS` | `ON` | Build automated tests |
| `VIDEO_EDITOR_BUILD_DESKTOP_UI` | `ON` | Build the Qt desktop application |
| `VIDEO_EDITOR_BUILD_MEDIA` | `ON` | Build FFmpeg, render, audio, proxy, and export modules |
| `VIDEO_EDITOR_BUILD_WORKERS` | `ON` | Build the worker-host executable |
| `VIDEO_EDITOR_ENABLE_GPU_RENDERING` | `ON` | Build libplacebo support when its exact dependency is found |
| `VIDEO_EDITOR_MINIAUDIO_ROOT` | empty | Prefix or directory containing `miniaudio.h` |
| `VIDEO_EDITOR_ENABLE_WHISPER_CPP` | `OFF` | Link the pinned optional local transcription backend |
| `VIDEO_EDITOR_TRANSCRIPTION_WHISPER_INCLUDE_DIR` | empty | Directory containing pinned `whisper.h` |
| `VIDEO_EDITOR_TRANSCRIPTION_WHISPER_LIBRARY` | empty | Pinned whisper.cpp library file |
| `VIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED` | `OFF` | Assert build provenance only when that linked backend was built with Vulkan |

To build faster without tests:

```bash
cmake --preset release -DVIDEO_EDITOR_BUILD_TESTS=OFF
cmake --build --preset release -j"$(nproc)"
```

## 4. Verify the build

Run the ordinary and sanitizer suites before interactive testing:

```bash
ctest --preset dev --output-on-failure

cmake --preset asan
cmake --build --preset asan -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=0 ctest --preset asan --output-on-failure
```

The accelerated endurance simulations are opt-in:

```bash
VE_RUN_LONG_TESTS=1 ctest --test-dir build/dev \
  -R '^(XrunValidation|DriftValidation)' --output-on-failure
```

These simulate one hour without xruns and two hours with less than one frame of accumulated drift;
they do not replace real-time validation on the supported physical device/driver matrix.

Inspect the linked FFmpeg runtime:

```bash
./build/dev/src/media_codec/video_editor_dependency_audit
```

`expected_abi` must be `true`. A distributable build also requires
`lgpl_compatible_configuration` to be `true`.

Optional capability checks:

```bash
ffmpeg -hide_banner -encoders | grep -E 'libvpx-vp9|libopus|vp9_vaapi'
vulkaninfo --summary
```

## 5. Run

```bash
./build/dev/src/app/VideoEditor
```

Run from a terminal while testing so stderr diagnostics are preserved. To install only the
application target into a local prefix:

```bash
cmake --install build/release --prefix ./install
./install/bin/VideoEditor
```

For a headless shell smoke test:

```bash
QT_QPA_PLATFORM=offscreen \
  ./build/dev/src/app/VideoEditor --screenshot editor.png
```

---

## Interactive alpha checklist

Record whether each item passed, failed, or was unavailable on the test machine.

### 1. Project save, reopen, and recovery

1. Create a project and save it as `.veproj`.
2. Import media and make several timeline edits.
3. Save, close normally, and reopen the project.
4. For a disposable project, terminate the process after completing another edit but before a
   checkpoint save, then restart the editor and inspect the recovery offer.

**Expected:** Canonical project and timeline edits survive save/reopen. A valid newer recovery
candidate is explained in plain language and can be opened without corrupting the saved checkpoint.
Media is referenced, not embedded, so original paths must remain available. Reopen rebuilds
offline/changed/proxy status, rediscovers matching cached proxies, and shows thumbnails/waveforms
when the cache still has them. Include any remaining offline or missing-bin state in the test report.

### 2. Media import

1. Import files through **File > Import Media**.
2. Drop media on the program viewer.
3. Search the media bin by name and format.
4. Insert a file containing linked video and audio.
**Expected:** Import runs asynchronously; successful items show name, duration, format, and status.
Linked A/V inserts atomically on compatible targeted tracks. Do not require desktop thumbnails,
waveform display, metadata editing, or relinking yet—those workflows remain incomplete.

### 3. Professional timeline interaction

Test all of the following with undo and redo:

- Ctrl-click multi-selection and Shift-click range selection;
- move, normal trim, ripple trim, and overwrite trim;
- roll, slip, and slide tools;
- linked A/V split, delete, and ripple delete;
- exact one-frame and ten-frame nudges;
- snapping to the playhead, markers, clip edges, and sequence frame grid;
- track create, rename, reorder, remove, lock, visibility, and targeting;
- marker create, move, rename, and remove;
- select and close a nonterminal gap;
- press Escape during a drag to cancel it.

**Expected:** A gesture previews transiently and commits one atomic revision on release. Invalid or
locked operations reject the complete batch. Multi-selection geometry and linked relationships are
preserved, and undo reverses the entire gesture in one step.

### 4. Titles, transitions, speed, effects, and keyframes

1. Add a title and edit its text, font, size, alignment, bold, and italic controls.
2. Add Cross Dissolve and Dip to Black transitions, drag their duration, change preset, and remove.
3. Set constant speed between 0.01× and 100× and toggle Reverse on a media clip.
4. Add color, crop, and blur effects.
5. Add Hold, Linear, and Bezier keyframes; edit exact time/value fields and drag the curve.
6. Save, close, reopen, and verify the authored state.

**Expected:** Every change is revisioned, undoable, schema-v3 persistent, and visible through the CPU
reference renderer. Unsupported title, transition, blend, or effect frames fall back from GPU to CPU
without disabling later compatible GPU frames.

### 5. Preview, transport, and GPU fallback

1. Play forward at 1×, pause/resume, seek, and scrub.
2. Use J/K/L, frame stepping, Home/End, and reverse/non-1× shuttle.
3. Exercise transform, crop, opacity, rotation, and blend modes.
4. If Vulkan/libplacebo is available, compare compatible GPU frames with CPU-fallback frames.

**Expected:** Frames continue updating during playback. Forward 1× follows the audio-master clock
when physical audio starts; otherwise the UI reports silent timer fallback. Reverse and non-1×
shuttle are currently silent. GPU-compatible frames use Vulkan; effects, titles, transitions,
non-Normal blends, or a moved rotation pivot use per-frame CPU fallback. Native swapchain
presentation and zero-copy decode are not implemented.

### 6. Realtime audio and professional mixing

This test requires a build that reported the miniaudio adapter as enabled.

1. Select **System default**, then a named output device.
2. Play a linked A/V clip forward at 1× and verify audible output follows the video.
3. Adjust clip and track gain/pan, mute, solo, fades, EQ, compressor, dialogue noise reduction, and
   limiter.
4. Observe per-track peak/RMS plus master peak/RMS and `LUFS-I`.
5. Change the loudness target, choose **Analyze loudness**, review the proposal, and apply it.
6. If safe, disconnect and reconnect a nonessential USB output while playback is intended.

**Expected:** The device callback remains responsive, meters follow the audible audio-master
position, LUFS shows analyzing/stale state until valid, and normalization applies one undoable atomic
track-gain batch. Device loss is detected within roughly one second, pauses safely, and a returned
selected/default endpoint recovers only while playback is still intended. Report any xrun, audible
glitch, restart after Pause/Stop, or growing A/V offset.

### 7. Captions and local transcription

1. Import UTF-8 SRT and WebVTT files, then add, search, edit, and delete cues.
2. Edit alignment, vertical position, safe margin, colors, emphasis, and outline.
3. Save/reopen the project and export SRT and WebVTT sidecars.
4. In Deliver, test styled caption burn-in and burn-in plus sidecar.
5. Choose the explicit model download. Cancel once during transfer and, if practical, once while it
   says `Verifying model`, then complete it. Verify the panel reports ready only after exact size and
   digest checks and that cancellation leaves no `.staging-*` directory in the model cache.
6. Select a speech clip, start local transcription, cancel once, then complete a new job. Activate a
   timed word and verify exact transcript navigation.
7. Review `Measured silence` and `Transcript filler` items. Verify measured silence starts selected,
   fillers start unselected, individual toggles work, Apply is one undo step, and an intervening edit
   makes the old review stale.

**Expected:** Caption words, provenance, and style are undoable and schema-v3 persistent. Sidecars
round to millisecond timing and write atomically. Burn-in honors the canonical style. Model bytes
are size-bounded while streaming, staged outside the project, verified off the UI thread, and a
cancel/mismatch is discarded. Transcription runs in a restartable
worker without uploading media. Accepted captions plus selected cuts commit atomically; cancel,
failure, discard, or a stale revision leaves the project unchanged. A build configured without
`VIDEO_EDITOR_ENABLE_WHISPER_CPP=ON` must truthfully report the backend unavailable.

### 8. Proxy workflow

1. Select media marked **Proxy recommended**.
2. Choose **Create editing proxy**, test cancellation once, then allow a new job to finish.
3. Play the timeline with proxy use enabled and export the project.

**Expected:** A half-resolution ProRes Proxy/MOV with PCM audio is produced when available, otherwise
the configured FFV1/Matroska fallback is used. Partial canceled output is not committed. Preview may
use the proxy, but export remains authoritative to originals. Reopening the project rediscovers a
matching cached proxy. 4K long-GOP media may start proxy generation automatically.

### 9. Reference and creator export

Test these Deliver presets:

- FFV1/Matroska reference master;
- ProRes 422 HQ/MOV when the encoder is available;
- YouTube 1080p, 1440p, and 2160p VP9/Opus WebM;
- vertical 1080×1920 and 720×1280 VP9/Opus WebM;
- Opus-only podcast WebM.

Also test custom resolution, exact frame rate, video bitrate or VP9 quality, audio bitrate, captions,
cancellation, and refusal to overwrite an existing destination without permission.

**Expected:** Reference masters contain exact 48 kHz stereo PCM. Creator video contains VP9 plus
optional Opus; podcast output has one Opus stream and no synthetic video. On Linux, enabled hardware
delivery may use `vp9_vaapi`; hardware setup/upload/encode failure must visibly restart the complete
atomic export with `libvpx-vp9`. Cancellation or failure must leave no partial destination. H.264
and AAC are intentionally unavailable.

### 10. Stability, accessibility, and Linux integration

Use the editor continuously for at least 20 minutes while importing, editing, playing, changing
workspaces, normalizing, and exporting. Test both Wayland and X11 where practical. Resize and dock
panels, use keyboard navigation, and inspect terminal diagnostics and memory growth.

**Expected:** No crash, freeze, runaway memory, corrupted project, stuck export, or lingering worker
process after exit. Major controls have accessible names and usable keyboard focus. Record the
session type, desktop environment, GPU/driver, audio backend/device, and whether CPU or GPU preview
was active.

---

## Known alpha limitations

Do not report these as regressions unless behavior is worse than described:

- Forward 1× physical audio requires the pinned miniaudio header and a usable device. Reverse and
  non-1× playback are silent. Hardware latency is estimated rather than calibrated.
- Hot-plug recovery uses one-second polling rather than native OS device events. Physical one-hour
  xrun and two-hour A/V drift evidence is still required across the supported matrix.
- GPU preview downloads an offscreen libplacebo result into Qt; there is no native swapchain,
  zero-copy decode, complete GPU effect/color parity, or HDR mastering.
- H.264/AAC export is disabled. Creator delivery currently uses FOSS VP9/Opus WebM.
- Local transcription requires the separately built pinned whisper.cpp backend and an on-demand
  approximately 141 MiB base model. Physical multilingual accuracy and Vulkan acceleration remain
  lab checks. Worker death is covered by automated host SIGKILL/stub-exit tests.
- Media-bin thumbnails, timeline waveforms, Inspector metadata, Relink, persistent proxy discovery,
  and **File > Manage Media Cache…** are implemented. Physical unplug remains a lab check.
- Source-monitor insert/overwrite editing, shortcut remapping, full LUT/color breadth, and the
  timed human accessibility/beginner study remain incomplete.
- Proxy, export, and transcription run in a restartable worker host. Release Flatpak identity and
  checksummed sources remain. A synthetic 200+ corpus generator exists. Signed Windows MSI and the
  Windows GPU matrix are deferred and should not be reported as Linux beta blockers.

## Reporting issues

Run the application from a terminal and include the following:

```text
Summary:

Steps to reproduce:
1.
2.
3.

Expected result:

Actual result:

Severity:
[ ] Crash
[ ] Data loss or project corruption
[ ] Playback/audio/export failure
[ ] Incorrect edit or render
[ ] UI/accessibility problem
[ ] Minor/cosmetic

Reproducibility:
[ ] Every time
[ ] Intermittent
[ ] Once

Build revision:
git rev-parse HEAD

System information:
uname -a
cat /etc/os-release
cmake --version
c++ --version
echo "$XDG_SESSION_TYPE"

GPU information:
vulkaninfo --summary

Audio information:
State whether miniaudio was enabled at configure time and identify the selected output device.

Terminal output / logs:
```

For playback problems, say whether audio was audible, whether the viewer title reported GPU or CPU
fallback, the media codec/resolution/frame rate, and whether a proxy was active. For export problems,
include the preset, destination container, actual encoder shown by the UI, fallback message, caption
mode, and whether an existing destination remained unchanged.

## Troubleshooting

### Qt 6.11.1 is not found

```bash
cmake --preset dev \
  -DCMAKE_PREFIX_PATH="/absolute/path/to/Qt/6.11.1/gcc_64;$VIDEO_EDITOR_DEPS"
```

### FFmpeg version mismatch

The source currently requires FFmpeg 9.0.1 library ABIs exactly. Check which pkg-config files are
visible:

```bash
pkg-config --modversion \
  libavformat libavcodec libavutil libswresample libswscale
```

Then correct `PKG_CONFIG_PATH`, delete or use a fresh build preset directory, and configure again.

### No audio during forward 1× playback

Check the CMake configure output. If miniaudio was unavailable, provide `miniaudio.h` 0.11.25 and
reconfigure with `-DVIDEO_EDITOR_MINIAUDIO_ROOT=/absolute/prefix`. If it was enabled, select
**System default** or a named device and inspect the visible playback error before checking mixer
gain and operating-system volume.

### Vulkan preview is unavailable

```bash
vulkaninfo --summary
```

The app remains usable through CPU rendering. Verify that libplacebo 7.360.1 and Vulkan headers were
visible during configuration if GPU testing is required.

### Creator presets are disabled

```bash
ffmpeg -hide_banner -encoders | grep -E 'libvpx-vp9|libopus|vp9_vaapi'
```

`libvpx-vp9` is required for creator video and `libopus` is required when the preset contains audio.
VAAPI is optional; its failure should fall back to libvpx.

### Transcription says the backend is unavailable

Manual captions still work. To test inference, build whisper.cpp 1.9.2 at commit
`306c88f4d1286aec1bf96e544632897886af5501`, then
configure with explicit include/library paths:

```bash
cmake --preset dev \
  -DVIDEO_EDITOR_ENABLE_WHISPER_CPP=ON \
  -DVIDEO_EDITOR_TRANSCRIPTION_WHISPER_INCLUDE_DIR=/absolute/path/to/whisper.cpp/include \
  -DVIDEO_EDITOR_TRANSCRIPTION_WHISPER_LIBRARY=/absolute/path/to/libwhisper.so
```

Add `-DVIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED=ON` only if that exact library was built and linked
with its Vulkan backend. This records a compiled capability; upstream whisper.cpp does not expose a
runtime-used Vulkan assertion through this adapter. The model is downloaded only after the tester
requests it in the application.

### Offscreen smoke test fails

Ensure the Qt platform plugins and shared dependency libraries are discoverable through
`CMAKE_PREFIX_PATH`, `QT_PLUGIN_PATH`, and the local runtime library path. Run with
`QT_DEBUG_PLUGINS=1` for Qt plugin diagnostics.

---

This alpha is intended to find correctness, reliability, and usability defects before public beta.
