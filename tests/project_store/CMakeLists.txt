# SPDX-License-Identifier: MPL-2.0

add_executable(video_editor_project_store_tests
  project_store_test.cpp
)

target_link_libraries(video_editor_project_store_tests
  PRIVATE
    video_editor::project_store
    video_editor::project_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(video_editor_project_store_tests)

target_sources(video_editor_project_store_tests PRIVATE checkpoint_fault_test.cpp)
