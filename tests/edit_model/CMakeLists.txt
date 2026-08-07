# SPDX-License-Identifier: MPL-2.0
find_package(GTest CONFIG REQUIRED)

add_executable(video_editor_edit_model_tests
  entity_id_test.cpp
  precision_edit_test.cpp
  time_test.cpp
  timeline_editor_test.cpp
)
target_link_libraries(video_editor_edit_model_tests
  PRIVATE
    VideoEditor::EditModel
    GTest::gtest_main
)
target_compile_features(video_editor_edit_model_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(video_editor_edit_model_tests)
