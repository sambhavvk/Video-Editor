# SPDX-License-Identifier: MPL-2.0
find_package(GTest CONFIG REQUIRED)

add_executable(video_editor_project_codec_tests project_codec_test.cpp)
target_link_libraries(video_editor_project_codec_tests
  PRIVATE
    VideoEditor::ProjectCodec
    GTest::gtest_main
)
target_compile_features(video_editor_project_codec_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(video_editor_project_codec_tests)
