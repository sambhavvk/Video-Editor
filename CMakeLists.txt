# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.30)

project(
  VideoEditor
  VERSION 0.1.0
  DESCRIPTION "Offline-first professional creator video editor"
  LANGUAGES C CXX)

# Exact timeline arithmetic currently relies on the portable Clang/GCC
# __int128 extension. LLVM clang-cl provides it with the Windows MSVC ABI;
# Microsoft's compiler does not.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  message(FATAL_ERROR
    "Windows builds currently require LLVM clang-cl so exact 128-bit timeline arithmetic is available")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ProjectOptions)
include(DependencyVersions)

option(VIDEO_EDITOR_BUILD_TESTS "Build automated tests" ON)
option(VIDEO_EDITOR_BUILD_DESKTOP_UI "Build the Qt desktop application" ON)
option(VIDEO_EDITOR_BUILD_MEDIA "Build the FFmpeg media modules" ON)
option(VIDEO_EDITOR_BUILD_WORKERS "Build background worker executables" ON)

video_editor_configure_project()

find_package(Threads REQUIRED)

add_custom_target(
  sbom
  COMMAND
    ${CMAKE_COMMAND}
    -DVIDEO_EDITOR_SBOM_OUTPUT=${CMAKE_BINARY_DIR}/artifacts/video-editor.spdx.json
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSbom.cmake
  COMMENT "Generating SPDX software bill of materials")

if(VIDEO_EDITOR_BUILD_TESTS)
  include(CTest)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
endif()

add_subdirectory(src/edit_model)
add_subdirectory(src/project_codec)
add_subdirectory(src/project_store)

if(VIDEO_EDITOR_BUILD_MEDIA)
  add_subdirectory(src/media_codec)
  add_subdirectory(src/caption_service)
  add_subdirectory(src/asset_service)
  add_subdirectory(src/media_cache)
  add_subdirectory(src/proxy_service)
  add_subdirectory(src/render_engine)
  add_subdirectory(src/playback)
  add_subdirectory(src/audio_engine)
  add_subdirectory(src/audio_render)
  add_subdirectory(src/export_service)
endif()

add_subdirectory(src/job_service)
if(VIDEO_EDITOR_BUILD_MEDIA)
  add_subdirectory(src/transcription_service)
endif()

if(VIDEO_EDITOR_BUILD_DESKTOP_UI)
  add_subdirectory(src/desktop_ui)
  add_subdirectory(src/app)
endif()

if(VIDEO_EDITOR_BUILD_WORKERS)
  add_subdirectory(src/workers)
endif()

if(VIDEO_EDITOR_BUILD_TESTS)
  add_subdirectory(tests)
endif()
