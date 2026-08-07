# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(video_editor_desktop_ui_tests_standalone LANGUAGES CXX)
    enable_testing()
endif()

if(NOT TARGET video_editor_desktop_ui)
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../../src/desktop_ui"
        "${CMAKE_CURRENT_BINARY_DIR}/desktop_ui"
    )
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS Test Widgets)

add_executable(video_editor_desktop_ui_tests
    editor_window_test.cpp
)

target_compile_features(video_editor_desktop_ui_tests PRIVATE cxx_std_20)
target_link_libraries(video_editor_desktop_ui_tests
    PRIVATE
        video_editor_desktop_ui
        Qt6::Test
        Qt6::Widgets
)

set_target_properties(video_editor_desktop_ui_tests PROPERTIES
    AUTOMOC ON
    CXX_EXTENSIONS OFF
)

add_test(NAME desktop_ui.editor_window COMMAND video_editor_desktop_ui_tests)
set_tests_properties(desktop_ui.editor_window PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 30
)

