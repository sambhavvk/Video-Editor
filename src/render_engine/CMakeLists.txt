# SPDX-License-Identifier: MPL-2.0

add_library(video_editor_render_engine
  src/cpu_renderer.cpp
  src/frame.cpp
  src/render_cache.cpp)

add_library(video_editor::render_engine ALIAS video_editor_render_engine)

target_include_directories(
  video_editor_render_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_render_engine
  PUBLIC video_editor::project_options video_editor_edit_model
  PRIVATE video_editor::project_warnings)
