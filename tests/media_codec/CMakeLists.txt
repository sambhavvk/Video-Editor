# SPDX-License-Identifier: MPL-2.0

add_executable(video_editor_media_codec_tests
  probe_test.cpp)

target_link_libraries(
  video_editor_media_codec_tests
  PRIVATE
    video_editor::media_codec
    video_editor::project_warnings
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(video_editor_media_codec_tests)
