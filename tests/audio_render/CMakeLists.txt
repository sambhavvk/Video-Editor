# SPDX-License-Identifier: MPL-2.0

find_program(VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)

add_executable(video_editor_audio_render_tests
  timeline_audio_renderer_test.cpp)

target_compile_definitions(
  video_editor_audio_render_tests
  PRIVATE
    VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE="${VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE}")

target_link_libraries(
  video_editor_audio_render_tests
  PRIVATE
    video_editor::audio_render
    video_editor::project_warnings
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(video_editor_audio_render_tests)
