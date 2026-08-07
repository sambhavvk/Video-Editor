# SPDX-License-Identifier: MPL-2.0

add_executable(video_editor_worker_host
  worker_main.cpp)

target_link_libraries(
  video_editor_worker_host
  PRIVATE
    video_editor::job_service
    video_editor::media_codec
    video_editor::project_warnings)
