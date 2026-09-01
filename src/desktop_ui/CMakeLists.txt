# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(video_editor_desktop_ui_standalone VERSION 0.1.0 LANGUAGES CXX)
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Widgets)

add_library(video_editor_desktop_ui STATIC
    include/video_editor/desktop_ui/cache_browser_dialog.hpp
    include/video_editor/desktop_ui/command_palette.hpp
    include/video_editor/desktop_ui/editor_window.hpp
    include/video_editor/desktop_ui/export_dialog.hpp
    include/video_editor/desktop_ui/keyboard_shortcuts_dialog.hpp
    include/video_editor/desktop_ui/keyframe_curve_widget.hpp
    include/video_editor/desktop_ui/shortcut_bindings.hpp
    include/video_editor/desktop_ui/panel_widgets.hpp
    include/video_editor/desktop_ui/program_viewer.hpp
    include/video_editor/desktop_ui/timeline_widget.hpp
    include/video_editor/desktop_ui/ui_types.hpp
    src/cache_browser_dialog.cpp
    src/command_palette.cpp
    src/editor_window.cpp
    src/export_dialog.cpp
    src/keyboard_shortcuts_dialog.cpp
    src/keyframe_curve_widget.cpp
    src/shortcut_bindings.cpp
    src/panel_widgets.cpp
    src/program_viewer.cpp
    src/timeline_widget.cpp
)

add_library(VideoEditor::DesktopUi ALIAS video_editor_desktop_ui)

target_include_directories(video_editor_desktop_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(video_editor_desktop_ui PUBLIC cxx_std_20)
target_link_libraries(video_editor_desktop_ui
    PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets
    PRIVATE video_editor::media_codec VideoEditor::ExportService)

set_target_properties(video_editor_desktop_ui PROPERTIES
    AUTOMOC ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_options(video_editor_desktop_ui PRIVATE /W4 /permissive-)
else()
    target_compile_options(video_editor_desktop_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()
