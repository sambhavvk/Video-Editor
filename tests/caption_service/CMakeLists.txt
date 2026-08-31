# SPDX-License-Identifier: MPL-2.0
find_package(GTest CONFIG REQUIRED)
find_program(VIDEO_EDITOR_TEST_FFMPEG NAMES ffmpeg)

add_executable(video_editor_caption_service_tests
  caption_service_test.cpp
  embedded_extract_test.cpp
  reflow_search_test.cpp
  transcript_proposal_test.cpp)
target_compile_features(video_editor_caption_service_tests PRIVATE cxx_std_20)
target_link_libraries(video_editor_caption_service_tests
  PRIVATE
    VideoEditor::CaptionService
    video_editor::media_codec
    GTest::gtest_main)

if(VIDEO_EDITOR_TEST_FFMPEG)
  target_compile_definitions(video_editor_caption_service_tests
    PRIVATE VIDEO_EDITOR_TEST_FFMPEG="${VIDEO_EDITOR_TEST_FFMPEG}")
endif()

if(TARGET video_editor::project_warnings)
  target_link_libraries(video_editor_caption_service_tests
    PRIVATE video_editor::project_warnings)
endif()

include(GoogleTest)
gtest_discover_tests(video_editor_caption_service_tests)
