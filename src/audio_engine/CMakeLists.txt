# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)
pkg_check_modules(EBUR128 REQUIRED IMPORTED_TARGET libebur128=1.2.6)

add_library(video_editor_audio_engine
  src/async_realtime_playback.cpp
  src/audio_block.cpp
  src/dsp.cpp
  src/loudness_meter.cpp
  src/miniaudio_output_device.cpp
  src/realtime_playback.cpp
  src/spsc_audio_ring.cpp)

add_library(video_editor::audio_engine ALIAS video_editor_audio_engine)

target_include_directories(
  video_editor_audio_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_audio_engine
  PUBLIC video_editor::project_options
  PRIVATE video_editor::project_warnings PkgConfig::EBUR128)

set(VIDEO_EDITOR_MINIAUDIO_ROOT "" CACHE PATH
    "Directory containing the pinned miniaudio 0.11.25 header")
find_path(VIDEO_EDITOR_MINIAUDIO_INCLUDE_DIR
  NAMES miniaudio.h
  HINTS "${VIDEO_EDITOR_MINIAUDIO_ROOT}" ENV MINIAUDIO_ROOT
  PATH_SUFFIXES include)

if(VIDEO_EDITOR_MINIAUDIO_INCLUDE_DIR)
  target_include_directories(video_editor_audio_engine SYSTEM PRIVATE
    "${VIDEO_EDITOR_MINIAUDIO_INCLUDE_DIR}")
  target_compile_definitions(video_editor_audio_engine PRIVATE
    VIDEO_EDITOR_HAS_MINIAUDIO=1)
  message(STATUS "Realtime audio: miniaudio 0.11.25 adapter enabled")
else()
  target_compile_definitions(video_editor_audio_engine PRIVATE
    VIDEO_EDITOR_HAS_MINIAUDIO=0)
  message(STATUS "Realtime audio: miniaudio adapter unavailable; manual callback fallback enabled")
endif()
