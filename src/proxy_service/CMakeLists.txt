# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)

pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample=${VIDEO_EDITOR_SWRESAMPLE_VERSION})
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale=${VIDEO_EDITOR_SWSCALE_VERSION})

add_library(video_editor_proxy_service
  src/proxy_service.cpp)

add_library(VideoEditor::ProxyService ALIAS video_editor_proxy_service)
add_library(video_editor::proxy_service ALIAS video_editor_proxy_service)

target_include_directories(
  video_editor_proxy_service
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_proxy_service
  PUBLIC
    video_editor::project_options
    video_editor::asset_service
    video_editor::media_cache
  PRIVATE
    video_editor::project_warnings
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWRESAMPLE
    PkgConfig::SWSCALE)

set_target_properties(video_editor_proxy_service PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
