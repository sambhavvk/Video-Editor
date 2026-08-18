# SPDX-License-Identifier: MPL-2.0

find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Core Concurrent Gui Widgets Network)

add_library(video_editor_app_controller
  editor_controller.cpp
  editor_controller.hpp
  media_reconstruction.cpp
  media_reconstruction.hpp
  path_utils.hpp
  worker_host_session.cpp
  worker_host_session.hpp)

target_compile_features(video_editor_app_controller PUBLIC cxx_std_20)
target_include_directories(video_editor_app_controller
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(
  video_editor_app_controller
  PUBLIC
    video_editor::project_options
    VideoEditor::DesktopUi
    video_editor_edit_model
    video_editor::asset_service
    video_editor::audio_engine
    video_editor::audio_render
    VideoEditor::CaptionService
    video_editor::job_service
  PRIVATE
    video_editor::project_warnings
    video_editor::project_store
    video_editor::project_codec
    video_editor::playback
    video_editor::proxy_service
    video_editor::media_cache
    video_editor::render_engine
    VideoEditor::ExportService
    video_editor::transcription_service
    Qt6::Network
    Qt6::Concurrent)

set_target_properties(video_editor_app_controller PROPERTIES AUTOMOC ON)

add_executable(video_editor main.cpp)
target_link_libraries(video_editor PRIVATE video_editor_app_controller video_editor::media_codec)
set_target_properties(video_editor PROPERTIES
  OUTPUT_NAME "VideoEditor"
  WIN32_EXECUTABLE ON)

install(TARGETS video_editor RUNTIME DESTINATION bin BUNDLE DESTINATION .)
