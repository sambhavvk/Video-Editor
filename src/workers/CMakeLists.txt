# SPDX-License-Identifier: MPL-2.0

add_library(video_editor_worker_dispatch
  job_dispatch.cpp)

add_library(VideoEditor::WorkerDispatch ALIAS video_editor_worker_dispatch)
add_library(video_editor::worker_dispatch ALIAS video_editor_worker_dispatch)

target_include_directories(
  video_editor_worker_dispatch
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(
  video_editor_worker_dispatch
  PRIVATE
    video_editor::media_codec
    video_editor::proxy_service
    video_editor::transcription_service
    VideoEditor::ExportService
    video_editor::playback
    video_editor::render_engine
    video_editor::audio_render
    video_editor::project_codec
    VideoEditor::EditModel
    video_editor::project_warnings
  PUBLIC
    video_editor::job_service
    video_editor::project_options)

add_executable(video_editor_worker_host
  worker_main.cpp)

target_link_libraries(
  video_editor_worker_host
  PRIVATE
    video_editor::worker_dispatch
    video_editor::transcription_service
    video_editor::project_warnings)

install(TARGETS video_editor_worker_host RUNTIME DESTINATION bin)
