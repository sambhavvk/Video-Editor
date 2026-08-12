# SPDX-License-Identifier: MPL-2.0

add_executable(video_editor_media_cache_tests
  cache_store_test.cpp
  metadata_service_test.cpp
  thumbnail_service_test.cpp
  waveform_service_test.cpp)

target_link_libraries(
  video_editor_media_cache_tests
  PRIVATE
    video_editor::media_cache
    video_editor::project_warnings
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(video_editor_media_cache_tests)
