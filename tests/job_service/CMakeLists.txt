# SPDX-License-Identifier: MPL-2.0

add_executable(video_editor_job_service_tests
  job_service_test.cpp)

target_link_libraries(
  video_editor_job_service_tests
  PRIVATE
    video_editor::job_service
    video_editor::project_warnings
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(video_editor_job_service_tests)
