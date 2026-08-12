# Video Editor — Alpha Test (Linux)

> **Version:** 0.1.0-alpha  
> **Status:** Pre-release, not for public distribution  
> **Last updated:** 2026-08-10

Thanks for helping test! This document walks you through building and running the video editor from source on Linux. It's a bit involved because we pin exact dependency versions — but once it's built, it just works.

---

## System Requirements

- **OS:** Ubuntu 24.04+, Fedora 41+, Arch Linux, or similar (glibc 2.38+)
- **RAM:** 8 GB minimum, 16 GB recommended
- **Disk:** ~10 GB free (build tree + dependencies)
- **GPU:** Not required. Optional Vulkan-capable GPU enables a libplacebo-accelerated preview path; CPU-only preview is fully supported.
- **Compiler:** GCC 14+ or Clang 18+ (must support C++20 with `__int128`)

---

## 1. Install System Packages

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git \
  libgl-dev libegl-dev libxkbcommon-dev \
  libvulkan-dev mesa-vulkan-drivers \
  python3 python3-pip
```

### Fedora

```bash
sudo dnf install -y \
  gcc gcc-c++ cmake ninja-build pkgconf git \
  mesa-libGL-devel mesa-libEGL-devel libxkbcommon-devel \
  vulkan-loader-devel mesa-vulkan-drivers \
  python3 python3-pip
```

### Arch Linux

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf git \
  mesa libxkbcommon \
  vulkan-icd-loader vulkan-tools \
  python
```

---

## 2. Install Exact-Version Dependencies

The project requires **exact** versions of several libraries. If your distro doesn't ship them, you'll need to build them from source or use a tool like vcpkg. The required versions are:

| Dependency     | Required Version |
|----------------|-----------------|
| Qt             | 6.11.1 (exact)  |
| FFmpeg         | 8.1.2 (exact)   |
| Protobuf       | 35.1 (exact)    |
| Abseil         | 20250512.1      |
| OpenSSL        | ≥ 3.0           |
| SQLite         | ≥ 3.45          |
| libplacebo     | 7.360.1         |
| libebur128     | 1.2.6           |
| GTest          | (for tests)     |

### Recommended: Use vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
cd ~/vcpkg
./bootstrap-vcpkg.sh

# Install what vcpkg can handle
./vcpkg install protobuf abseil openssl 'sqlite3[json1]' gtest libplacebo libebur128
```

> ⚠️ **Qt 6.11.1** must be installed separately — it's too new for most distros and vcpkg. Use the [Qt Online Installer](https://www.qt.io/download-qt-installer-oss) (open-source edition), install to `~/Qt/6.11.1/gcc_64`, and set `CMAKE_PREFIX_PATH`.
>
> ⚠️ **FFmpeg 8.1.2** — if your distro doesn't have it, build from source with `--enable-shared` or grab a pre-built bundle.

Then configure with:
```bash
cmake --preset release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.1/gcc_64;$HOME/vcpkg/installed/x64-linux" \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

### Manual build from source

If you prefer not to use vcpkg, build each dependency from source. See `cmake/DependencyVersions.cmake` for the exact versions needed. Install each to a local prefix (e.g. `build/deps/`) and point `CMAKE_PREFIX_PATH` there.

---

## 3. Build

```bash
cd /path/to/VideoEditor

# Configure (Release build)
cmake --preset release

# Build
cmake --build build/release -j$(nproc)
```

This produces the `VideoEditor` executable at `build/release/src/app/VideoEditor`.

### Build options

| CMake Variable                   | Default | Description                        |
|----------------------------------|---------|------------------------------------|
| `VIDEO_EDITOR_BUILD_TESTS`       | ON      | Build the test suite               |
| `VIDEO_EDITOR_BUILD_DESKTOP_UI`  | ON      | Build the Qt desktop app           |
| `VIDEO_EDITOR_BUILD_MEDIA`       | ON      | Build FFmpeg media modules         |
| `VIDEO_EDITOR_BUILD_WORKERS`     | ON      | Build background worker executables|

To build without tests (faster):
```bash
cmake --preset release -DVIDEO_EDITOR_BUILD_TESTS=OFF
cmake --build build/release -j$(nproc)
```

---

## 4. Run

```bash
./build/release/src/app/VideoEditor
```

Or install to a prefix and run from there:
```bash
cmake --install build/release --prefix ./install
./install/bin/VideoEditor
```

---

## 5. Run Tests (Optional)

```bash
cd build/release
ctest --output-on-failure
```

---

## What to Test

Work through each item below. Each has an **Expected Result** so you know what a passing test looks like.

### 1. Project creation

Create a new project, save it, close the application, and reopen the saved project file.

**Expected result:** The project opens with all settings intact. No errors on save or load. The file round-trips cleanly.

### 2. Media import

Import video files using drag-and-drop from a file manager **and** using the in-app file picker.

**Expected result:** Imported files appear in the media bin with generated thumbnails. Thumbnail generation completes within a few seconds for typical clips.

### 3. Timeline editing

Insert clips from the media bin into the timeline. Try moving, trimming, splitting, and ripple-deleting clips. Test undo and redo after each operation.

**Expected result:** All edit operations behave correctly. Undo reverses the last action; redo re-applies it. The timeline state stays consistent after a sequence of edits and undos.

### 4. Video preview

Play back the timeline using the preview controls.

**Expected result:** Video frames render asynchronously using the CPU pipeline. Playback is smooth for standard-definition content; some frame drops on high-resolution clips are acceptable. **No audio will play through speakers** — realtime audio-device playback is not integrated in this alpha. Audio data is rendered internally and included in exports, but you cannot audition it live.

### 5. Captions

Import an SRT or WebVTT subtitle file. Verify it appears in the caption track. Try searching caption text, editing a caption entry (add, modify text, delete), and exporting a subtitle file.

**Expected result:** The caption file loads and displays in a dedicated caption track. Search finds matching text. Edits persist after save/reload. Exported SRT or WebVTT matches your edits (round-trip).

### 6. Proxy workflow

Select a media clip in the media bin and trigger proxy generation. Wait for it to complete, then play back the timeline. Switch the clip back to original media.

**Expected result:** A half-resolution proxy file is created. Timeline preview uses the proxy automatically (you should see lower-resolution output). Switching back to original media restores full resolution.

### 7. Reference export

Open the **Deliver** workspace and export a master file. Use the default settings.

**Expected result:** Export produces an FFV1/Matroska (`.mkv`) or ProRes/MOV (`.mov`) file. **The output is video-only with 48 kHz stereo PCM audio** (per ADR 0009). No H.264/AAC creator presets are available. Open the exported file in VLC or `ffplay` and verify it plays back correctly with both video and audio.

### 8. Inspector properties

Select a clip on the timeline and open the Inspector panel. Adjust transform properties (position, scale, rotation, anchor point), crop, opacity, blend mode, clip gain, pan, and fades. Save the project, close, reopen, and check the properties again.

**Expected result:** Changes are visible in the preview as you make them. After save and reload, all property values are preserved exactly as you set them.

### 9. Stability

Use the editor for 15–20 minutes of continuous work — import media, edit the timeline, preview, export. Watch for crashes, freezes, excessive memory use, or zombie processes.

**Expected result:** No segfaults, hangs, or runaway memory. The application remains responsive. No lingering `VideoEditor` or worker processes after quitting.

### 10. UI

Examine every panel and workspace. Look for overlapping widgets, unreadable text (wrong colors, clipped labels), broken layouts at different window sizes, or missing accessible labels (try navigating with keyboard only).

**Expected result:** All text is readable. Widgets don't overlap. Layouts adapt reasonably to window resizing. Screen readers can identify major UI elements.

---

## Known Limitations

These are deliberate boundaries of the 0.1.0-alpha, not bugs:

- **No realtime audio-device playback** — Timeline audio is rendered for export but you cannot hear it through speakers during preview.
- **No integrated GPU presentation** — The preview path is CPU-rendered. An optional libplacebo backend exists for testing but native swapchain presentation is not wired up.
- **No H.264/AAC creator export** — Only FFV1/Matroska and ProRes/MOV reference masters. Creator codecs are a legal/packaging gate.
- **No automatic proxy scheduling** — Proxies are manually triggered only.
- **No titles, transitions, effects, or color tools** — Model fields may exist in the UI but no complete authoring or render workflow is wired up.
- **No local transcription** — The whisper.cpp worker is not integrated.
- **No caption burn-in** — Captions export as standalone SRT/WebVTT files only; they are not burned into the video stream.

---

## Reporting Issues

When something breaks, please include the following. The more detail, the faster we can fix it.

### Issue template

```
**Summary:**
One-line description of the problem.

**Steps to Reproduce:**
1. ...
2. ...
3. ...

**Expected Result:**
What should have happened.

**Actual Result:**
What actually happened.

**Severity:**
[ ] Crash (application terminates or segfaults)
[ ] Data loss (project file corruption, lost edits)
[ ] Visual (layout, rendering, or display problem)
[ ] Minor (cosmetic, inconvenience)

**System Info:**
Paste the output of:
  uname -a && cmake --version && gcc --version | head -1 && cat /etc/os-release | head -4

**Build Version:**
Paste the output of:
  git rev-parse HEAD

**Logs / Terminal Output:**
Paste any error messages from the terminal, or attach the relevant
lines from terminal stderr. Check build/release/ for crash dump files
and attach them if present.
```

### Tips

- Run the app from a terminal so you can see stderr output — many errors print there.
- After a crash, check `build/release/` for core dumps or crash dump files and include them.
- If you can reproduce the issue reliably, that's the most valuable thing you can tell us.

---

## Troubleshooting

**"Qt6 not found"** — Set `CMAKE_PREFIX_PATH` to your Qt install:
```bash
cmake --preset release -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/gcc_64
```

**"FFmpeg version mismatch"** — You need the exact version. See the vcpkg instructions above or build from source.

**"clang required"** — GCC works fine on Linux. This error only applies to Windows.

**No Vulkan drivers / optional GPU path not working** — Vulkan is only needed for the optional libplacebo preview acceleration. The editor works fine without it. If you want to test the GPU path, verify your drivers:
```bash
vulkaninfo | head -20
```

**Smoke test without a display** — You can verify the app launches and renders without a physical display:
```bash
QT_QPA_PLATFORM=offscreen ./build/release/src/app/VideoEditor --screenshot editor.png
```
This writes a screenshot of the initial editor state to `editor.png`. If it produces an image, the app started successfully.

---

*This is an alpha build. Expect rough edges. Thank you for testing!*
