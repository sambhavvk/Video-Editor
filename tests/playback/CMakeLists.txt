# SPDX-License-Identifier: MPL-2.0

find_program(VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)

add_executable(video_editor_playback_tests
  ffmpeg_frame_provider_test.cpp)

target_compile_definitions(
  video_editor_playback_tests
  PRIVATE
    VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE="${VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE}")

target_link_libraries(
  video_editor_playback_tests
  PRIVATE
    video_editor::playback
    video_editor::project_warnings
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(video_editor_playback_tests)
