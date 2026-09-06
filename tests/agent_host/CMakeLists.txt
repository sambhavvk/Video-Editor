# SPDX-License-Identifier: MPL-2.0

find_package(GTest CONFIG REQUIRED)
find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Core)
find_program(VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)

add_executable(video_editor_agent_host_tests
  agent_host_test.cpp)

target_compile_definitions(
  video_editor_agent_host_tests
  PRIVATE
    VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE="${VIDEO_EDITOR_TEST_FFMPEG_EXECUTABLE}")

target_link_libraries(
  video_editor_agent_host_tests
  PRIVATE
    video_editor::agent_host_lib
    video_editor::agent_protocol
    GTest::gtest_main
    Qt6::Core
    video_editor::project_warnings)

include(GoogleTest)
gtest_discover_tests(video_editor_agent_host_tests)
