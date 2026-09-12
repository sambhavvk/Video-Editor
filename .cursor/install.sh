#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Cloud Agent environment bootstrap for the Video-Editor core engine.
#
# The full desktop application pins bleeding-edge, exact dependency versions
# (Qt 6.11.1, FFmpeg 9.0.1, libplacebo 7.360.1, ...) that are not available as
# distribution packages, so this script provisions the dependency-light
# "core-only" configuration: the timeline edit model, caption service, project
# codec/store, and job service, plus their tests. See ALPHA-TEST-LINUX.md for
# the full-application dependency contract.
#
# The script is idempotent: it is safe to re-run.
set -euo pipefail

CMAKE_VERSION="3.30.5"
CMAKE_PREFIX="/opt/cmake-${CMAKE_VERSION}"
PROTOBUF_SHIM_DIR="/usr/local/lib/cmake/protobuf"

log() { printf '\n=== %s ===\n' "$*"; }

log "Installing system build dependencies"
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -y
sudo apt-get install -y --no-install-recommends \
  build-essential g++ gcc ninja-build pkg-config git curl ca-certificates python3 \
  libgtest-dev libgmock-dev libsqlite3-dev \
  protobuf-compiler libprotobuf-dev libabsl-dev

# The default `c++`/`cc` alternatives point at clang, whose C++ driver cannot
# locate libstdc++ in this image. The project documents GCC as the recommended
# compiler and relies on the __int128 extension, so make GCC the default.
log "Selecting GCC as the default C/C++ compiler"
if update-alternatives --list c++ 2>/dev/null | grep -q '/usr/bin/g++$'; then
  sudo update-alternatives --set c++ /usr/bin/g++
fi
if update-alternatives --list cc 2>/dev/null | grep -q '/usr/bin/gcc$'; then
  sudo update-alternatives --set cc /usr/bin/gcc
fi

# CMake 3.30+ is required (Ubuntu 24.04 ships 3.28). Install the official
# binary distribution to a fixed prefix and expose it on PATH.
if ! command -v cmake >/dev/null 2>&1 || \
   ! cmake --version | head -1 | grep -Eq '3\.(3[0-9]|[4-9][0-9])'; then
  log "Installing CMake ${CMAKE_VERSION}"
  if [ ! -x "${CMAKE_PREFIX}/bin/cmake" ]; then
    tmp="$(mktemp -d)"
    curl -fsSL -o "${tmp}/cmake.tar.gz" \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz"
    sudo rm -rf "${CMAKE_PREFIX}"
    sudo mkdir -p "${CMAKE_PREFIX}"
    sudo tar --strip-components=1 -xzf "${tmp}/cmake.tar.gz" -C "${CMAKE_PREFIX}"
    rm -rf "${tmp}"
  fi
  echo "export PATH=${CMAKE_PREFIX}/bin:\$PATH" | sudo tee /etc/profile.d/cmake-video-editor.sh >/dev/null
  sudo ln -sf "${CMAKE_PREFIX}/bin/cmake" /usr/local/bin/cmake
  sudo ln -sf "${CMAKE_PREFIX}/bin/ctest" /usr/local/bin/ctest
  sudo ln -sf "${CMAKE_PREFIX}/bin/cpack" /usr/local/bin/cpack
fi
export PATH="${CMAKE_PREFIX}/bin:${PATH}"

# Debian/Ubuntu ship protobuf without an upstream CMake config package, but the
# project calls find_package(Protobuf CONFIG). Install a shim that delegates to
# CMake's bundled FindProtobuf module so CONFIG lookups resolve to the system
# protobuf (which matches the installed protoc).
log "Installing protobuf CMake CONFIG shim"
sudo mkdir -p "${PROTOBUF_SHIM_DIR}"
sudo tee "${PROTOBUF_SHIM_DIR}/protobuf-config.cmake" >/dev/null <<'EOF'
# Shim protobuf CONFIG package delegating to CMake's FindProtobuf module.
set(protobuf_MODULE_COMPATIBLE TRUE)
include(FindProtobuf)
find_package(Protobuf MODULE REQUIRED)
set(Protobuf_FOUND TRUE)
set(protobuf_FOUND TRUE)
EOF
protoc_version="$(protoc --version 2>/dev/null | awk '{print $2}')"
sudo tee "${PROTOBUF_SHIM_DIR}/protobuf-config-version.cmake" >/dev/null <<EOF
set(PACKAGE_VERSION "${protoc_version:-0.0.0}")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
set(PACKAGE_VERSION_EXACT FALSE)
EOF

# Configure and build the core-only engine and run its test suite so a fresh
# environment lands ready to iterate.
log "Configuring and building the core-only engine"
cd "$(dirname "$0")/.."
cmake --preset core-only
cmake --build --preset core-only -j"$(nproc)"

log "Environment ready. Run tests with: ctest --preset core-only"
