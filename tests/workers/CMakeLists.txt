# SPDX-License-Identifier: MPL-2.0

find_program(VIDEO_EDITOR_WORKER_TEST_FFMPEG NAMES ffmpeg)

add_executable(video_editor_worker_tests
  worker_dispatch_test.cpp)

target_link_libraries(
  video_editor_worker_tests
  PRIVATE
    video_editor::worker_dispatch
    video_editor::job_service
    video_editor::proxy_service
    video_editor::project_warnings
    GTest::gtest_main)

if(VIDEO_EDITOR_WORKER_TEST_FFMPEG)
  target_compile_definitions(
    video_editor_worker_tests
    PRIVATE VIDEO_EDITOR_WORKER_TEST_FFMPEG="${VIDEO_EDITOR_WORKER_TEST_FFMPEG}")
endif()

include(GoogleTest)
gtest_discover_tests(video_editor_worker_tests)
