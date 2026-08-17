# SPDX-License-Identifier: MPL-2.0

find_program(VIDEO_EDITOR_TEST_FFMPEG NAMES ffmpeg)

add_executable(video_editor_media_cache_tests
  cache_store_test.cpp
  ffmpeg_generation_test.cpp
  metadata_service_test.cpp
  thumbnail_service_test.cpp
  waveform_service_test.cpp)

target_link_libraries(
  video_editor_media_cache_tests
  PRIVATE
    video_editor::media_cache
    video_editor::project_warnings
    GTest::gtest_main)

if(VIDEO_EDITOR_TEST_FFMPEG)
  target_compile_definitions(
    video_editor_media_cache_tests
    PRIVATE VIDEO_EDITOR_TEST_FFMPEG="${VIDEO_EDITOR_TEST_FFMPEG}")
endif()

include(GoogleTest)
gtest_discover_tests(video_editor_media_cache_tests)
