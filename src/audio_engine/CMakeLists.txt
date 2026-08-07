# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)
pkg_check_modules(EBUR128 REQUIRED IMPORTED_TARGET libebur128=1.2.6)

add_library(video_editor_audio_engine
  src/audio_block.cpp
  src/dsp.cpp
  src/loudness_meter.cpp
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
