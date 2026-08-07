# Video Editor — Alpha Test (Windows)

> **Version:** 0.1.0-alpha  
> **Status:** Pre-release, not for public distribution  
> **Last updated:** 2026-08-07

Thanks for helping test! This document walks you through building and running the video editor from source on Windows. It requires LLVM clang-cl (not Microsoft's MSVC) because the project uses `__int128` for timeline arithmetic.

---

## System Requirements

- **OS:** Windows 10 (22H2+) or Windows 11
- **RAM:** 8 GB minimum, 16 GB recommended
- **Disk:** ~15 GB free (tools + deps + build tree)
- **GPU:** Any GPU with Vulkan support (for rendering pipeline)

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

## What to Test

Focus areas for this alpha:

- [ ] **Project creation** — Can you create a new project and save it?
- [ ] **Media import** — Drag-and-drop or file picker to import video/audio files
- [ ] **Timeline** — Add clips, rearrange, trim, split
- [ ] **Playback** — Does the preview play correctly? Audio in sync?
- [ ] **Export** — Can you export a finished video?
- [ ] **Crashes** — Does anything crash, freeze, or eat all your RAM?
- [ ] **UI quirks** — Does anything look broken, overlap, or misalign?

---

## Reporting Issues

When something breaks, please tell Sambhav:

1. **What you did** (steps to reproduce)
2. **What happened** (error message, crash, visual glitch)
3. **What you expected**
4. **System info** — run this in PowerShell and paste the output:
   ```powershell
   systeminfo | Select-Object "OS Name","OS Version","Total Physical Memory"
   cmake --version
   clang-cl --version
   ```
5. **Logs** — check terminal output for error messages

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

---

*This is an alpha build. Expect rough edges. Thank you for testing!*
