<!-- SPDX-License-Identifier: MPL-2.0 -->

# Video Editor — Linux beta tester guide

This beta is **Linux only**. Windows is out of scope. The editor is offline-first and does not
require an account. Keep original media and backups of important projects.

For day-to-day editing, see the [user guide](docs/user-guide.md). For what is implemented versus
still incomplete, see [beta feature status](docs/beta-feature-status.md).

---

## Session logger

Every launch writes a **local** session log. Nothing is uploaded. The log exists so a crash or
stuck action can be reconstructed from UI clicks and a few backend events.

### Where it lives

```text
~/.local/share/VideoEditor/VideoEditor/logs/
```

| File | Meaning |
| --- | --- |
| `session-YYYYMMDD-HHMMSS.log` | One file per launch (UTC timestamp in the name) |
| `current.session` | Pointer to the active log; removed on a clean exit |
| `last-crash.log` | Pointer written if the process dies with a crash signal |

If the last session crashed, the next launch shows a dialog with the saved log path.

### What is collected

Plain-text lines with a UTC timestamp. Typical categories:

| Kind | Examples | What it contains |
| --- | --- | --- |
| `SESSION start` | path to this log file | Local filesystem path only |
| `UI click` | `widget=playButton class=QPushButton button=left` | Qt object name (or a few named ancestors), widget class, mouse button. **No click coordinates, no keystrokes, no text you typed** |
| `BACKEND importPaths` | `count=3` | Number of files, **not** their paths or contents |
| `BACKEND setPlaybackRate` | `rate=1` | Numeric transport rate |
| `BACKEND analyzeLoudnessNormalization` | `start` | That Analyze was requested |
| `BACKEND apply` / `applyBatch` | `success op=Add asset` or `failure count=2` | High-level edit operation name or batch size, success/failure |
| `BACKEND startAudioMasterPlayback` | `success` or `failure unavailable` | Whether 1× audio output started |
| `SHUTDOWN clean` | written on a normal exit | Confirms the process shut down without a crash |
| `CRASH signal=11` | written by the crash handler | POSIX signal number only (for example SIGSEGV) |

### What is not collected

- Media, audio, thumbnails, or project file contents
- Import/export filesystem paths
- Account data (there is none)
- Network activity (the app does not send these logs anywhere)

Attach the session log when you report a crash or a mixer/playback failure. You can open it in any
text editor before sharing and delete older files in that folder at any time.

---

## Build and run on your Linux PC

Validated on current glibc x86-64 distros. You need **CMake 3.30+**, Ninja, a C++20 compiler
(GCC or Clang), pkg-config, Git, Python 3, and the pinned libraries below. 8 GB RAM minimum;
16 GB and 15 GB of disk for the build tree are more comfortable.

The source expects **exact** versions for several libraries (see
[build and dependencies](docs/developer/build-and-dependencies.md)). At this revision they are
Qt **6.11.1**, FFmpeg **9.0.1**, libplacebo **7.360.1**, libebur128 **1.2.6**, Protobuf **35.1**,
and miniaudio **0.11.25** (fetched automatically during configure if the header is missing).

### 1. System packages

**Arch / CachyOS**

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf git python \
  qt6-base ffmpeg protobuf abseil-cpp libplacebo libebur128 \
  sqlite openssl gtest vulkan-headers vulkan-icd-loader \
  mesa libxkbcommon vulkan-tools
```

**Ubuntu / Debian**

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git python3 \
  libgl-dev libegl-dev libxkbcommon-dev \
  libvulkan-dev mesa-vulkan-drivers vulkan-tools
```

Then provide Qt 6.11.1, FFmpeg 9.0.1, and the other pinned libraries from a local prefix if the
distro packages are not those exact versions.

**Fedora**

```bash
sudo dnf install -y \
  gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git python3 \
  mesa-libGL-devel mesa-libEGL-devel libxkbcommon-devel \
  vulkan-loader-devel mesa-vulkan-drivers vulkan-tools
```

Same note: distro packages must match the pinned contracts, or point CMake at a prefix that does.

### 2. Configure and build

From the repository root:

```bash
cmake --preset dev
cmake --build --preset dev -j"$(nproc)"
```

Watch configure for:

- `Realtime audio: miniaudio 0.11.25 adapter enabled` — hardware playback can be tested
- `miniaudio adapter unavailable; manual callback fallback enabled` — preview will be silent

The desktop binary is:

```text
build/dev/src/app/VideoEditor
```

Faster, without tests:

```bash
cmake --preset release -DVIDEO_EDITOR_BUILD_TESTS=OFF
cmake --build --preset release -j"$(nproc)"
```

If Qt or FFmpeg live in a custom prefix:

```bash
export CMAKE_PREFIX_PATH="/absolute/path/to/Qt/6.11.1/gcc_64:/absolute/path/to/deps"
export PKG_CONFIG_PATH="$CMAKE_PREFIX_PATH/lib/pkgconfig:$CMAKE_PREFIX_PATH/lib64/pkgconfig"
cmake --preset dev -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"
```

### 3. Check the build

```bash
ctest --preset dev --output-on-failure
./build/dev/src/media_codec/video_editor_dependency_audit
```

`expected_abi` should be `true`.

### 4. Run

Always start from a terminal so FFmpeg/Qt messages stay visible:

```bash
./build/dev/src/app/VideoEditor
```

Optional smoke test without a display session:

```bash
QT_QPA_PLATFORM=offscreen \
  ./build/dev/src/app/VideoEditor --screenshot /tmp/editor.png
```

---

## What to try

Work through the [user guide](docs/user-guide.md). Useful Linux-beta checks:

1. Create, save, close, and reopen a `.veproj`.
2. Import a few clips (including an MKV/WebM if you have one), edit the timeline, undo/redo.
3. Play forward at 1× with the Audio Mixer on **System default** and confirm audible output.
4. Change a mixer fader with the mouse wheel; the editor must not crash.
5. Run **Analyze** loudness, then apply or cancel.
6. Export a short FOSS VP9/Opus WebM from Deliver.

For release-style audio sync validation (not required for casual smoke testing), see the
[physical-device A/V lab protocol](docs/quality/testing-and-release-gates.md#physical-device-av-lab-protocol)
in the quality gates doc: one hour zero xruns, two hours drift below 10 ms, with per-device
calibration saved from the Audio Mixer.

After a session, glance at the newest file in
`~/.local/share/VideoEditor/VideoEditor/logs/` and confirm it has `SESSION start` and, if you quit
normally, `SHUTDOWN clean`.

---

## Reporting issues

Include:

```text
Summary:
Steps:
Expected:
Actual:
git rev-parse HEAD
uname -a
cat /etc/os-release
echo "$XDG_SESSION_TYPE"
```

Attach the matching `session-*.log` (and terminal stderr). For playback/mixer bugs, say whether
configure printed the miniaudio-enabled line and which output device was selected.

Do not treat these as regressions unless they are worse than documented: reverse / non-1× playback
is silent; H.264/AAC export is unavailable; GPU preview is an offscreen download into Qt, not a
native swapchain; local transcription needs an optional whisper.cpp build plus an explicit model
download.
