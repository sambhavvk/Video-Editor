# SPDX-License-Identifier: MPL-2.0
find_package(GTest CONFIG REQUIRED)
find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Core)

add_executable(video_editor_agent_protocol_tests edit_command_json_test.cpp)
target_link_libraries(video_editor_agent_protocol_tests
  PRIVATE
    video_editor_agent_protocol
    GTest::gtest_main
    Qt6::Core
)
target_compile_features(video_editor_agent_protocol_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(video_editor_agent_protocol_tests)
