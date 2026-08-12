# Video Editor — Alpha Test (Windows)

> **Version:** 0.1.0-alpha  
> **Status:** Pre-release, not for public distribution  
> **Last updated:** 2026-08-10

Thanks for helping test! This document walks you through building and running the video editor from source on Windows. It requires LLVM clang-cl (not Microsoft's MSVC) because the project uses `__int128` for timeline arithmetic.

---

## System Requirements

- **OS:** Windows 10 (22H2+) or Windows 11
- **RAM:** 8 GB minimum, 16 GB recommended
- **Disk:** ~15 GB free (tools + deps + build tree)
- **GPU:** Not required. Optional Vulkan-capable GPU enables a libplacebo-accelerated preview path (D3D11 on Windows); CPU-only preview is fully supported.

---

## 1. Install Required Tools

Install these in order. Each one is needed.

### 1a. Git for Windows

Download: https://git-scm.com/download/win

Default install options are fine.

### 1b. CMake 3.30+

Download: https://cmake.org/download/ → Windows x64 Installer

✅ Check **"Add CMake to the system PATH"** during install.

### 1c. Ninja

Download: https://github.com/ninja-build/ninja/releases

Get `ninja-win.zip`, extract `ninja.exe` somewhere, and add that folder to your PATH.  
Or install via `winget`:
```powershell
winget install Ninja-build.Ninja
```

### 1d. LLVM (clang-cl)

**This is required — MSVC will not work.**

Download: https://github.com/llvm/llvm-project/releases

Get the latest **LLVM 19+** Windows x64 installer (e.g. `LLVM-19.x.x-win64.exe`).

✅ Check **"Add LLVM to the system PATH for all users"** during install.

Verify:
```powershell
clang-cl --version
```

### 1e. Visual Studio Build Tools (for Windows SDK + linker)

You still need the MSVC **linker** and **Windows SDK**, just not the compiler.

Download: https://visualstudio.microsoft.com/downloads/ → "Build Tools for Visual Studio 2022"

In the installer, select:
- **MSVC v143 - VS 2022 C++ x64/x86 build tools**
- **Windows 11 SDK** (latest)

No need to install the full Visual Studio IDE.

### 1f. PowerShell 7 (for MSI packaging, optional)

```powershell
winget install Microsoft.PowerShell
```

---

## 2. Install Dependencies

The project requires **exact** versions of several libraries. You'll need to build or obtain them manually.

### Required Versions

| Dependency     | Required Version | Notes                          |
|----------------|-----------------|--------------------------------|
| Qt             | 6.11.1 (exact)  | Use Qt online installer        |
| FFmpeg         | 8.1.2 (exact)   | BtbN builds or compile from source |
| Protobuf       | 35.1 (exact)    | vcpkg or build from source     |
| Abseil         | 20250512.1      | vcpkg or build from source     |
| OpenSSL        | ≥ 3.0           | Strawberry Perl + build, or vcpkg |
| SQLite         | ≥ 3.45          | amalgamation download          |
| libplacebo     | 7.360.1         | Build from source              |
| libebur128     | 1.2.6           | Build from source              |
| GTest          | latest          | vcpkg or build from source     |

### Recommended: Use vcpkg for most deps

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# Install what vcpkg can handle
.\vcpkg install protobuf abseil openssl sqlite3 gtest libplacebo libebur128
```

> ⚠️ **Qt 6.11.1** must be installed separately via the [Qt Online Installer](https://www.qt.io/download-qt-installer-oss). Install the `MSVC 2022 64-bit` component to e.g. `C:\Qt\6.11.1\msvc2022_64`. Note: the project builds with clang-cl, but uses the MSVC-targeting Qt build.

> ⚠️ **FFmpeg 8.1.2** — grab pre-built dev + shared DLLs from [BtbN's GitHub releases](https://github.com/BtbN/FFmpeg-Builds/releases) (get `ffmpeg-n8.1.2-latest-win64-gpl-shared-8.1.zip`), or compile from source.

---

## 3. Build

Open **PowerShell** and run:

```powershell
cd C:\path\to\VideoEditor

# Configure — uses the windows-dev preset (clang-cl)
cmake --preset windows-dev `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.11.1\msvc2022_64;C:\vcpkg\installed\x64-windows" `
  -DVCPKG_TARGET_TRIPLET=x64-windows

# Build
cmake --build build/windows-dev -j $env:NUMBER_OF_PROCESSORS
```

This produces `build\windows-dev\src\app\VideoEditor.exe`.

### If you installed deps manually (not vcpkg)

Point CMake to each dependency:
```powershell
cmake --preset windows-dev `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.11.1\msvc2022_64" `
  -DFFMPEG_ROOT="C:\deps\ffmpeg" `
  -DProtobuf_ROOT="C:\deps\protobuf" `
  -Dabsl_ROOT="C:\deps\abseil" `
  -DOPENSSL_ROOT_DIR="C:\deps\openssl" `
  -DSQLite3_ROOT="C:\deps\sqlite" `
  -Dlibplacebo_ROOT="C:\deps\libplacebo" `
  -DEBUR128_ROOT="C:\deps\libebur128"
```

---

## 4. Run

```powershell
.\build\windows-dev\src\app\VideoEditor.exe
```

**Important:** Qt and FFmpeg DLLs must be on the PATH or next to the exe. Quick way:
```powershell
$env:PATH = "C:\Qt\6.11.1\msvc2022_64\bin;C:\deps\ffmpeg\bin;$env:PATH"
.\build\windows-dev\src\app\VideoEditor.exe
```

Or copy the required DLLs next to `VideoEditor.exe`:
- `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6Concurrent.dll`
- `avcodec-62.dll`, `avformat-62.dll`, `avutil-60.dll`, `swresample-6.dll`, `swscale-9.dll`
- `libcrypto-3-x64.dll`, `libssl-3-x64.dll`
- Any platform plugins: `platforms/qwindows.dll`

---

## 5. Run Tests (Optional)

```powershell
cd build\windows-dev
ctest --output-on-failure
```

---

## Test Checklist

Walk through each item below. Check it off when it passes. If something fails, see [Reporting Issues](#reporting-issues).

### 1. Project creation

- [ ] Create a new project.
- [ ] Save the project.
- [ ] Close and reopen the editor.
- [ ] Reopen the saved project and verify everything loads correctly.

**Expected result:** The project file round-trips cleanly — all your settings and media references survive save/close/reopen without errors or missing data.

### 2. Media import

- [ ] Drag a video file from Explorer into the media bin.
- [ ] Use the file picker to import a second video file.
- [ ] Verify both clips appear in the media bin with thumbnails.

**Expected result:** Imported files show up in the media bin with visible thumbnail frames. No errors in the terminal.

### 3. Timeline editing

- [ ] Insert clips from the media bin onto the timeline.
- [ ] Move a clip to a different position.
- [ ] Trim (drag the edge of) a clip.
- [ ] Split a clip at the playhead.
- [ ] Ripple-delete a clip and verify the gap closes.
- [ ] Test undo (Ctrl+Z) and redo (Ctrl+Y) for each operation above.

**Expected result:** Every edit operation works as described. Undo/redo reverses and re-applies each change correctly without corruption.

### 4. Video preview

- [ ] Place clips on the timeline and press Play.
- [ ] Scrub the timeline by dragging the playhead.
- [ ] Verify that video frames update in the preview panel.

**Expected result:** The preview shows async CPU-rendered video frames from your timeline. Frame updates may not be real-time on slower machines — that's expected.

> ⚠️ **No audio will play through speakers.** Realtime audio-device playback is not integrated. Audio data *is* rendered and exported, but you cannot audition it live during preview. This is a deliberate alpha boundary.

### 5. Captions

- [ ] Import an SRT or WebVTT subtitle file.
- [ ] Verify it appears in the caption track on the timeline.
- [ ] Search within the caption text.
- [ ] Edit a caption entry (add text, modify text, delete an entry).
- [ ] Export a subtitle file and verify the content matches your edits.

**Expected result:** Captions load into a dedicated track, are searchable, and are fully editable. Exported SRT/WebVTT files round-trip correctly — what you edit is what you get.

### 6. Proxy workflow

- [ ] Select a media clip in the bin.
- [ ] Trigger proxy generation (right-click or menu).
- [ ] Verify the half-resolution proxy file is created.
- [ ] Confirm the timeline preview uses the proxy.
- [ ] Switch back to original media and confirm the preview updates.

**Expected result:** A proxy file is generated at half resolution. The timeline uses it for preview, and you can toggle between proxy and original without issues.

### 7. Reference export

- [ ] Open the **Deliver** workspace.
- [ ] Configure and export a master file.
- [ ] Open the exported file in VLC or `ffplay`.

**Expected result:** The export produces an FFV1/Matroska (`.mkv`) or ProRes/MOV (`.mov`) file. **Export is video-only with 48 kHz stereo PCM audio** (per ADR 0009). There are no H.264/AAC creator presets. The output file plays correctly with both video and audio in VLC or ffplay.

### 8. Inspector properties

- [ ] Select a clip on the timeline.
- [ ] In the Inspector, adjust **transform** properties: position, scale, rotation, anchor point.
- [ ] Adjust **crop**, **opacity**, and **blend mode**.
- [ ] Adjust **clip gain**, **pan**, and **fade in/out**.
- [ ] Verify each change appears in the preview.
- [ ] Save the project, close, reopen, and verify the properties survived round-trip.

**Expected result:** Every property adjustment is visible in the preview and persists across save/reload. No values reset or get lost.

### 9. Stability

- [ ] Use the editor for at least 15–20 minutes with multiple clips.
- [ ] Watch for crashes, freezes, or unresponsiveness.
- [ ] Check memory usage (Task Manager) — it should stay reasonable.

**Expected result:** The editor remains stable. No hard crashes, no UI freezes longer than a few seconds, no runaway memory growth.

### 10. UI polish

- [ ] Look for overlapping widgets or unreadable text.
- [ ] Check that layouts don't break at different window sizes.
- [ ] Verify that interactive elements have accessible labels (check with a screen reader or inspect tool if you're able).

**Expected result:** The UI is usable — no overlapping panels, no clipped text, no completely broken layouts.

---

## Known Limitations

These are **not bugs**. They are boundaries of the current alpha scope.

- **No realtime audio-device playback** — Timeline audio is rendered for export but you cannot hear it through speakers during preview. This is a deliberate alpha boundary, not a bug.
- **No integrated GPU presentation** — The preview path is CPU-rendered. An optional libplacebo backend exists for testing but native swapchain presentation is not wired up.
- **No H.264/AAC creator export** — Only FFV1/Matroska and ProRes/MOV reference masters. Creator codecs are a legal/packaging gate.
- **No automatic proxy scheduling** — Proxies are manually triggered only.
- **No titles, transitions, effects, or color tools** — Model fields may exist but no complete authoring/render workflow.
- **No local transcription** — The whisper.cpp worker is not integrated.
- **Caption burn-in** — Not available; captions export as standalone SRT/WebVTT files only.

---

## Reporting Issues

When something breaks, please report it using the template below. The more detail you provide, the faster we can fix it.

### Issue Template

```
**Summary:**
One-line description of the problem.

**Steps to Reproduce:**
1. Open the editor
2. ...
3. ...

**Expected Result:**
What you expected to happen.

**Actual Result:**
What actually happened. Include any error messages.

**Severity:**
[ ] Crash — the app closes or becomes completely unresponsive
[ ] Data loss — project files or media are corrupted or lost
[ ] Visual — layout, rendering, or display problems
[ ] Minor — cosmetic, inconvenience, or nice-to-have

**System Info:**
Paste the output of the PowerShell command below.

**Build Version:**
Paste the output of `git rev-parse HEAD` from the repo root.

**Logs / Terminal Output:**
Paste any error messages or warnings from the terminal where you launched the editor.
```

### Collecting System Info

Run this in PowerShell from the repo root and paste the output into your issue report:

```powershell
# System info
systeminfo | Select-Object "OS Name","OS Version","Total Physical Memory"

# Build tools
cmake --version
clang-cl --version

# Git commit (build version)
git rev-parse HEAD

# GPU info (optional — helps with libplacebo issues)
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion
```

---

## Troubleshooting

**"C++ compiler not found"** — Make sure LLVM is on PATH and you're using the `windows-dev` preset (which sets `clang-cl`).

**"MSVC is not supported"** — This is expected. You must use LLVM clang-cl. Install LLVM and ensure `clang-cl` is on PATH.

**"Qt6 not found"** — Set `CMAKE_PREFIX_PATH` to your Qt install:
```powershell
cmake --preset windows-dev -DCMAKE_PREFIX_PATH="C:\Qt\6.11.1\msvc2022_64"
```

**"DLL not found" at runtime** — Add Qt and FFmpeg bin directories to PATH, or copy DLLs next to the exe (see Step 4).

**"Protobuf version mismatch"** — Must be exactly 35.1. Check with `protoc --version`.

**"linker errors with __int128"** — You're probably using MSVC instead of clang-cl. Verify with `cmake --preset windows-dev` and check that the compiler is `clang-cl`.

**Offscreen smoke test** — If you can't launch the GUI (e.g. headless CI or display issues), you can verify the app starts and renders a frame without a display:
```powershell
$env:QT_QPA_PLATFORM='offscreen'
.\build\windows-dev\src\app\VideoEditor.exe --screenshot editor.png
```
This writes a single-frame screenshot to `editor.png` and exits. A successful run with no errors confirms the app initializes correctly.

---

*This is an alpha build. Expect rough edges. Thank you for testing!*
