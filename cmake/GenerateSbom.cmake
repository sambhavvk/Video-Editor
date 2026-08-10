# SPDX-License-Identifier: MPL-2.0

if(NOT DEFINED VIDEO_EDITOR_SBOM_OUTPUT)
  message(FATAL_ERROR "VIDEO_EDITOR_SBOM_OUTPUT is required")
endif()

# Keep this generated development SBOM tied to the same dependency contract as
# CMake configuration and the runtime audit. Do not duplicate version pins.
include("${CMAKE_CURRENT_LIST_DIR}/DependencyVersions.cmake")

get_filename_component(_sbom_directory "${VIDEO_EDITOR_SBOM_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_sbom_directory}")
string(TIMESTAMP _created "%Y-%m-%dT%H:%M:%SZ" UTC)

file(WRITE "${VIDEO_EDITOR_SBOM_OUTPUT}" [=[
{
  "SPDXID": "SPDXRef-DOCUMENT",
  "spdxVersion": "SPDX-2.3",
  "dataLicense": "CC0-1.0",
  "name": "VideoEditor-development-dependencies",
  "documentNamespace": "https://video-editor.invalid/spdx/development",
  "creationInfo": {
    "created": "]=] "${_created}" [=[",
    "creators": ["Tool: VideoEditor-CMake-SBOM"]
  },
  "packages": [
    {"SPDXID":"SPDXRef-VideoEditor","name":"VideoEditor","versionInfo":"0.1.0","licenseConcluded":"MPL-2.0"},
    {"SPDXID":"SPDXRef-Qt","name":"Qt","versionInfo":"]=] "${VIDEO_EDITOR_QT_VERSION}" [=[","licenseConcluded":"LGPL-3.0-only"},
    {"SPDXID":"SPDXRef-FFmpeg","name":"FFmpeg","versionInfo":"]=] "${VIDEO_EDITOR_FFMPEG_VERSION}" [=[","licenseConcluded":"NOASSERTION"},
    {"SPDXID":"SPDXRef-libplacebo","name":"libplacebo","versionInfo":"]=] "${VIDEO_EDITOR_LIBPLACEBO_VERSION}" [=[","licenseConcluded":"LGPL-2.1-or-later"},
    {"SPDXID":"SPDXRef-miniaudio","name":"miniaudio","versionInfo":"]=] "${VIDEO_EDITOR_MINIAUDIO_VERSION}" [=[","licenseConcluded":"MIT-0"},
    {"SPDXID":"SPDXRef-SQLite","name":"SQLite","versionInfo":"3.53.4","licenseConcluded":"blessing"},
    {"SPDXID":"SPDXRef-Abseil","name":"Abseil","versionInfo":"20250512.1","licenseConcluded":"Apache-2.0"},
    {"SPDXID":"SPDXRef-Protobuf","name":"Protocol Buffers","versionInfo":"35.1","licenseConcluded":"BSD-3-Clause"},
    {"SPDXID":"SPDXRef-OpenSSL","name":"OpenSSL","versionInfo":"3.6.3","licenseConcluded":"Apache-2.0"},
    {"SPDXID":"SPDXRef-libebur128","name":"libebur128","versionInfo":"1.2.6","licenseConcluded":"MIT"},
    {"SPDXID":"SPDXRef-GTest","name":"GoogleTest","versionInfo":"1.17.0","licenseConcluded":"BSD-3-Clause"}
  ]
}
]=])

message(STATUS "Wrote ${VIDEO_EDITOR_SBOM_OUTPUT}")
