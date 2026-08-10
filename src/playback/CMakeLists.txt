# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)

pkg_check_modules(PLAYBACK_AVFORMAT REQUIRED IMPORTED_TARGET
                  libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(PLAYBACK_AVCODEC REQUIRED IMPORTED_TARGET
                  libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(PLAYBACK_AVUTIL REQUIRED IMPORTED_TARGET
                  libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})
pkg_check_modules(PLAYBACK_SWSCALE REQUIRED IMPORTED_TARGET
                  libswscale=${VIDEO_EDITOR_SWSCALE_VERSION})

add_library(video_editor_playback
  src/asset_registry.cpp
  src/ffmpeg_frame_provider.cpp)

add_library(video_editor::playback ALIAS video_editor_playback)

target_include_directories(
  video_editor_playback
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_playback
  PUBLIC
    video_editor_media_codec
    video_editor_render_engine
    video_editor_edit_model
    video_editor::project_options
  PRIVATE
    video_editor::project_warnings
    PkgConfig::PLAYBACK_AVFORMAT
    PkgConfig::PLAYBACK_AVCODEC
    PkgConfig::PLAYBACK_AVUTIL
    PkgConfig::PLAYBACK_SWSCALE)

set_target_properties(video_editor_playback PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
