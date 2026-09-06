# SPDX-License-Identifier: MPL-2.0

find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Core)

add_library(video_editor_agent_host_lib
  src/agent_host.cpp)

add_library(video_editor::agent_host_lib ALIAS video_editor_agent_host_lib)

target_include_directories(
  video_editor_agent_host_lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_agent_host_lib
  PUBLIC
    video_editor::agent_protocol
    VideoEditor::EditModel
    video_editor::project_codec
    video_editor::project_store
    video_editor::asset_service
    video_editor::media_codec
    video_editor::playback
    video_editor::render_engine
    Qt6::Core
  PRIVATE
    video_editor::project_warnings)

add_executable(video_editor_agent_host
  src/main.cpp)

target_link_libraries(
  video_editor_agent_host
  PRIVATE
    video_editor::agent_host_lib
    video_editor::project_warnings)

install(TARGETS video_editor_agent_host RUNTIME DESTINATION bin)
