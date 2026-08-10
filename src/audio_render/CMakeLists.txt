# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)

pkg_check_modules(AUDIO_RENDER_AVFORMAT REQUIRED IMPORTED_TARGET
                  libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(AUDIO_RENDER_AVCODEC REQUIRED IMPORTED_TARGET
                  libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(AUDIO_RENDER_AVUTIL REQUIRED IMPORTED_TARGET
                  libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})
pkg_check_modules(AUDIO_RENDER_SWRESAMPLE REQUIRED IMPORTED_TARGET
                  libswresample=${VIDEO_EDITOR_SWRESAMPLE_VERSION})

add_library(video_editor_audio_render
  src/original_audio_registry.cpp
  src/timeline_audio_renderer.cpp)

add_library(video_editor::audio_render ALIAS video_editor_audio_render)

target_include_directories(
  video_editor_audio_render
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_audio_render
  PUBLIC
    video_editor::audio_engine
    video_editor_edit_model
    video_editor::project_options
  PRIVATE
    video_editor::project_warnings
    PkgConfig::AUDIO_RENDER_AVFORMAT
    PkgConfig::AUDIO_RENDER_AVCODEC
    PkgConfig::AUDIO_RENDER_AVUTIL
    PkgConfig::AUDIO_RENDER_SWRESAMPLE)

set_target_properties(video_editor_audio_render PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
