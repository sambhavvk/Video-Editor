# SPDX-License-Identifier: MPL-2.0
find_package(GTest CONFIG REQUIRED)

add_executable(video_editor_project_codec_tests project_codec_test.cpp)
target_link_libraries(video_editor_project_codec_tests
  PRIVATE
    VideoEditor::ProjectCodec
    GTest::gtest_main
)
target_include_directories(video_editor_project_codec_tests
  PRIVATE
    "${CMAKE_BINARY_DIR}/src/project_codec"
)
target_compile_features(video_editor_project_codec_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(video_editor_project_codec_tests)
