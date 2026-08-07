# Video Editor — Alpha Test (Linux)

> **Version:** 0.1.0-alpha  
> **Status:** Pre-release, not for public distribution  
> **Last updated:** 2026-08-07

Thanks for helping test! This document walks you through building and running the video editor from source on Linux. It's a bit involved because we pin exact dependency versions — but once it's built, it just works.

---

## System Requirements

- **OS:** Ubuntu 24.04+, Fedora 41+, Arch Linux, or similar (glibc 2.38+)
- **RAM:** 8 GB minimum, 16 GB recommended
- **Disk:** ~10 GB free (build tree + dependencies)
- **GPU:** Any GPU with Vulkan support (for rendering pipeline)
- **Compiler:** GCC 14+ or Clang 18+ (must support C++20 with `__int128`)

---

## 1. Install System Packages

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git \
  libgl-dev libegl-dev libxkbcommon-dev \
  libpulse-dev libasound2-dev \
  libvulkan-dev mesa-vulkan-drivers \
  libva-dev libvdpau-dev \
  python3 python3-pip
```

### Fedora

```bash
sudo dnf install -y \
  gcc gcc-c++ cmake ninja-build pkgconf git \
  mesa-libGL-devel mesa-libEGL-devel libxkbcommon-devel \
  pulseaudio-libs-devel alsa-lib-devel \
  vulkan-loader-devel mesa-vulkan-drivers \
  libva-devel libvdpau-devel \
  python3 python3-pip
```

### Arch Linux

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf git \
  mesa libxkbcommon \
  pulseaudio alsa-lib \
  vulkan-icd-loader vulkan-tools \
  libva libvdpau \
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

Focus areas for this alpha:

- [ ] **Project creation** — Can you create a new project and save it?
- [ ] **Media import** — Drag-and-drop or file picker to import video/audio files
- [ ] **Timeline** — Add clips, rearrange, trim, split
- [ ] **Playback** — Does the preview play correctly? Audio in sync?
- [ ] **Export** — Can you export a finished video?
- [ ] **Crashes** — Does anything segfault, freeze, or eat all your RAM?
- [ ] **UI quirks** — Does anything look broken, overlap, or misalign?

---

## Reporting Issues

When something breaks, please tell Sambhav:

1. **What you did** (steps to reproduce)
2. **What happened** (error message, crash, visual glitch)
3. **What you expected**
4. **System info** — run this and paste the output:
   ```bash
   uname -a && cmake --version && gcc --version | head -1 && cat /etc/os-release | head -4
   ```
5. **Logs** — check terminal output, or `build/release/` for any crash dumps

---

## Troubleshooting

**"Qt6 not found"** — Set `CMAKE_PREFIX_PATH` to your Qt install:
```bash
cmake --preset release -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/gcc_64
```

**"FFmpeg version mismatch"** — You need the exact version. See the vcpkg instructions above or build from source.

**"clang required"** — GCC works fine on Linux. This error only applies to Windows.

**Vulkan errors** — Make sure you have working Vulkan drivers:
```bash
vulkaninfo | head -20
```

---

*This is an alpha build. Expect rough edges. Thank you for testing!*
