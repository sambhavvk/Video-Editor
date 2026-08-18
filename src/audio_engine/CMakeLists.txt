# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)
pkg_check_modules(EBUR128 REQUIRED IMPORTED_TARGET libebur128=1.2.6)

add_library(video_editor_audio_engine
  src/async_realtime_playback.cpp
  src/audio_device_manager.cpp
  src/audio_block.cpp
  src/dialogue_denoise.cpp
  src/dsp.cpp
  src/loudness_meter.cpp
  src/miniaudio_output_device.cpp
  src/playback_meter.cpp
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
option(VIDEO_EDITOR_FETCH_MINIAUDIO
       "Fetch pinned miniaudio 0.11.25 when the header is not already available" ON)
find_path(VIDEO_EDITOR_MINIAUDIO_INCLUDE_DIR
  NAMES miniaudio.h
  HINTS
    "${VIDEO_EDITOR_MINIAUDIO_ROOT}"
    ENV MINIAUDIO_ROOT
    "${CMAKE_BINARY_DIR}/_deps/miniaudio-src"
    "${CMAKE_BINARY_DIR}/_deps/video_editor_miniaudio-src"
    "${CMAKE_SOURCE_DIR}/third_party/miniaudio"
  PATH_SUFFIXES include extras/miniaudio_split)

if(NOT VIDEO_EDITOR_MINIAUDIO_INCLUDE_DIR AND VIDEO_EDITOR_FETCH_MINIAUDIO)
  include(FetchContent)
  set(MINIAUDIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(MINIAUDIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(MINIAUDIO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(video_editor_miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(video_editor_miniaudio)
  if(DEFINED video_editor_miniaudio_SOURCE_DIR AND
     EXISTS "${video_editor_miniaudio_SOURCE_DIR}/miniaudio.h")
    set(VIDEO_EDITOR_MINIAUDIO_INCLUDE_DIR "${video_editor_miniaudio_SOURCE_DIR}"
        CACHE PATH "Pinned miniaudio 0.11.25 include directory" FORCE)
  endif()
endif()

if(VIDEO_EDITOR_MINIAUDIO_INCLUDE_DIR)
  target_include_directories(video_editor_audio_engine SYSTEM PRIVATE
    "${VIDEO_EDITOR_MINIAUDIO_INCLUDE_DIR}")
  target_compile_definitions(video_editor_audio_engine PRIVATE
    VIDEO_EDITOR_HAS_MINIAUDIO=1)
  target_link_libraries(video_editor_audio_engine PRIVATE
    Threads::Threads ${CMAKE_DL_LIBS})
  message(STATUS "Realtime audio: miniaudio 0.11.25 adapter enabled")
else()
  target_compile_definitions(video_editor_audio_engine PRIVATE
    VIDEO_EDITOR_HAS_MINIAUDIO=0)
  message(STATUS "Realtime audio: miniaudio adapter unavailable; manual callback fallback enabled")
endif()
