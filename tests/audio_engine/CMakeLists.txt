# SPDX-License-Identifier: MPL-2.0

add_executable(video_editor_audio_engine_tests
  audio_engine_test.cpp)

target_link_libraries(
  video_editor_audio_engine_tests
  PRIVATE
    video_editor::audio_engine
    video_editor::project_warnings
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(video_editor_audio_engine_tests)
