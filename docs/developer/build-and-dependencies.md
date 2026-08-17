<!-- SPDX-License-Identifier: MPL-2.0 -->

# Developer build and dependency guide

The repository uses C++20, CMake 3.30+, Ninja, and pinned library ABIs. It does not yet contain a
complete dependency bootstrap or vcpkg lock, so a developer must provide compatible packages.
System packages are acceptable for local work only when they match the source contracts; they are
not an official distribution bundle.

## Dependency contract

`cmake/DependencyVersions.cmake` is authoritative. At the time of this document the source expects:

| Dependency | Contract |
| --- | --- |
| Qt | 6.11.1 exact; Core, Concurrent, Gui, Widgets, Network |
| FFmpeg | 9.0.1 exact; libavformat/libavcodec 63.1.101, libavutil 61.1.101, libswresample 7.1.101, libswscale 10.1.101 |
| libplacebo | 7.360.1 exact; optional capability-gated D3D11/Vulkan backend with a truthful stub |
| miniaudio | 0.11.25 exact header; optional physical output adapter with a manual callback fallback |
| SQLite | 3.45 or newer |
| Protobuf | 35.1, with Abseil 20250512.1 in the release lock |
| OpenSSL | 3.0 or newer |
| libebur128 | 1.2.6 |
| whisper.cpp | 1.9.2 at `306c88f4d1286aec1bf96e544632897886af5501`; optional local transcription backend |
| GTest | package-config target required when tests are enabled |

Official packages must dynamically link approved LGPL Qt, FFmpeg, and libplacebo builds, with GPL
and nonfree FFmpeg options disabled. A successful local compile is not a licensing or patent
approval. Run the dependency audit and source/license gate before treating any binary as a release
candidate.

## CMake presets

| Preset | Purpose |
| --- | --- |
| `dev` | Full Debug build, tests enabled |
| `asan` | Debug with AddressSanitizer and UndefinedBehaviorSanitizer on non-MSVC toolchains |
| `release` | Full Release build |
| `core-only` | Edit, captions, project codec/store, and job protocol without Qt, FFmpeg media modules, or workers |
| `windows-dev` | Debug build using LLVM `clang-cl` and the MSVC ABI |

The feature switches default to `ON`: `VIDEO_EDITOR_BUILD_TESTS`,
`VIDEO_EDITOR_BUILD_DESKTOP_UI`, `VIDEO_EDITOR_BUILD_MEDIA`, and
`VIDEO_EDITOR_BUILD_WORKERS`. `VIDEO_EDITOR_ENABLE_GPU_RENDERING` also defaults to `ON`; this permits
probing for the exact libplacebo contract but does not make GPU support mandatory for a local build.

### Optional realtime and GPU backends

Set `MINIAUDIO_ROOT` or `VIDEO_EDITOR_MINIAUDIO_ROOT` to a directory whose `include/` contains the
exact miniaudio 0.11.25 single header. The compile performs a version assertion. If the header is
missing, `MiniaudioOutputDevice` remains available as a truthful `Unavailable` adapter and the
manual callback path remains testable; that build cannot provide audible desktop playback.

CMake prints either `Realtime audio: miniaudio 0.11.25 adapter enabled` or an explicit unavailable
message while configuring. If `build/dev` was first configured without the header, provide the
pinned source and reconfigure the existing build before launching it:

```sh
cmake --preset dev -DVIDEO_EDITOR_MINIAUDIO_ROOT=/absolute/path/to/miniaudio-0.11.25
cmake --build build/dev --target VideoEditor
```

Merely installing the header after configuration does not change an existing executable; CMake must
run again. A build reporting the manual callback fallback is intentionally silent in the desktop.

With `VIDEO_EDITOR_ENABLE_GPU_RENDERING=ON`, CMake links only the exact libplacebo version. Linux
also needs Vulkan headers and a Vulkan 1.2-capable runtime; the Windows backend requests D3D feature
level 11.0 or newer. If discovery or runtime initialization fails, the same API reports an
unavailable diagnostic and the CPU renderer remains mandatory. Use
`-DVIDEO_EDITOR_ENABLE_GPU_RENDERING=OFF` for an intentional CPU-only build.

The engine has an offscreen GPU timeline path which decodes active video clips, uploads individual
RGBA float frames, and applies crop, position, scale, custom anchor or centered-pivot rotation,
opacity, and Normal source-over composition through libplacebo. Rotation around a moved pivot,
effects, title clips, active transitions, or non-Normal blend modes fall back to CPU for the affected
frame. The desktop
requests that path before CPU and downloads the result for Qt; backend/device/upload/render/readback
failures preserve a CPU result and latch CPU preview for the session. A native Windows `HWND` or caller-owned Linux `VkInstance`/
`VkSurfaceKHR` is supported by the engine contract but is not yet supplied by the desktop. There is
no zero-copy decoder import. Official beta packages must contain and validate both the pinned
miniaudio adapter and the platform GPU backend; the fallbacks keep project open/edit/recovery and CPU
export usable, not release-ready.

### Optional local transcription backend

The worker and typed transcription protocol always build, but inference is deliberately disabled
unless the exact pinned whisper.cpp library is supplied. Enable it with:

```sh
cmake --preset dev \
  -DVIDEO_EDITOR_ENABLE_WHISPER_CPP=ON \
  -DVIDEO_EDITOR_TRANSCRIPTION_WHISPER_INCLUDE_DIR=/absolute/path/to/whisper.cpp/include \
  -DVIDEO_EDITOR_TRANSCRIPTION_WHISPER_LIBRARY=/absolute/path/to/libwhisper.so
```

Set `VIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED=ON` only when the linked library was built with its
Vulkan backend; this provenance assertion reports a compiled capability, does not make a missing
device usable, and does not claim that a particular inference used Vulkan. The desktop downloads
the pinned base model only after an explicit user action. The configured manifest
requires 147,951,465 bytes and SHA-1 `465707469ff3a37a2b9b8d8f89f2f99de7299dac`; staged bytes are
bounded during transfer and verified off the Qt thread before atomic installation. Verification is
cooperatively cancelable. Tests inject local fetchers and never download the live model.

## Linux development build

Install a C++20 compiler with `__int128` support, CMake, Ninja, pkg-config, and development packages
or a local prefix containing the exact contracts above. Then:

```sh
cmake --preset dev \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/dependency-prefix
cmake --build --preset dev
ctest --preset dev
```

If pkg-config files for FFmpeg or libebur128 are outside standard paths, set `PKG_CONFIG_PATH`
before configuring. Qt and Protobuf CMake package files must be discoverable through
`CMAKE_PREFIX_PATH` or their package-specific directory variables.

For engine work that does not need Qt or FFmpeg:

```sh
cmake --preset core-only \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/core-dependency-prefix
cmake --build --preset core-only
ctest --preset core-only
```

Run the application with:

```sh
./build/dev/src/app/VideoEditor
./build/dev/src/app/VideoEditor /absolute/path/to/media.mov
QT_QPA_PLATFORM=offscreen ./build/dev/src/app/VideoEditor --screenshot /tmp/editor.png
```

## Windows development build

Native Microsoft C++ compilation is rejected. Exact time comparison and rescaling currently use
128-bit integer intermediates; LLVM `clang-cl` supplies them while preserving the Windows MSVC ABI.
Install LLVM, Ninja, CMake 3.30+, Visual Studio Build Tools/Windows SDK, and x64 MSVC-targeting
dependency builds.

From a Developer PowerShell with `clang-cl` and Ninja on `PATH`:

```powershell
cmake --preset windows-dev `
  -DCMAKE_PREFIX_PATH="C:\deps\prefix;C:\Qt\6.11.1\msvc2022_64"
cmake --build --preset windows-dev
ctest --preset windows-dev
```

Runtime DLLs and the Qt `platforms/qwindows.dll` plugin must be on `PATH` or deployed beside the
executable. The current install target does not deploy the complete runtime dependency set; the MSI
is a packaging skeleton, not a distributable installer.

## Generated code and artifacts

Protobuf C++ sources are generated in the build tree for the project snapshot and worker protocol.
Do not commit generated build outputs. Caches, `.working.sqlite`, `-wal`, `-shm`, downloaded models,
and locally generated proxies are also non-source artifacts.

Generate the source-declared SPDX SBOM with:

```sh
cmake --build build/dev --target sbom
```

It is a source-lock artifact, not proof of the binaries loaded at runtime.

## Local verification

```sh
ctest --preset dev
cmake --build build/dev --target sbom
tools/quality/dependency_license_gate.sh --source-only
tools/quality/format_changed_cpp.sh --check
python3 tools/quality/verify_corpus.py
```

For an official candidate, also run the built `video_editor_dependency_audit` through
`dependency_license_gate.sh --official --audit ...`. The current release-source lock and corpus are
intentionally incomplete, so release-mode gates are expected to fail until those blockers are
resolved. See [Testing and release gates](../quality/testing-and-release-gates.md).

## Module and contract changes

Keep changes inside one module when possible. Public edit-model headers, snapshot schema, worker
protocol, dependency versions, and license configuration are protected contracts. Update or add an
ADR before changing them, include tests, and follow the repository's two-owner review policy.
