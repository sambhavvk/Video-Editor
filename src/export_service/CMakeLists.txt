# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)

pkg_check_modules(EXPORT_AVFORMAT REQUIRED IMPORTED_TARGET
                  libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(EXPORT_AVCODEC REQUIRED IMPORTED_TARGET
                  libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(EXPORT_AVUTIL REQUIRED IMPORTED_TARGET
                  libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})

add_library(video_editor_export_service
  src/export_service.cpp)

add_library(VideoEditor::ExportService ALIAS video_editor_export_service)

target_include_directories(
  video_editor_export_service
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_export_service
  PUBLIC
    video_editor_render_engine
    video_editor_edit_model
    video_editor::project_options
  PRIVATE
    video_editor::project_warnings
    PkgConfig::EXPORT_AVFORMAT
    PkgConfig::EXPORT_AVCODEC
    PkgConfig::EXPORT_AVUTIL)

set_target_properties(video_editor_export_service PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
