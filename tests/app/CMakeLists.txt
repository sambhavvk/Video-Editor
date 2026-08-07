# SPDX-License-Identifier: MPL-2.0

find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Test Widgets)

add_executable(video_editor_app_tests editor_controller_test.cpp)
target_compile_features(video_editor_app_tests PRIVATE cxx_std_20)
target_link_libraries(video_editor_app_tests
  PRIVATE
    video_editor_app_controller
    video_editor::project_warnings
    Qt6::Test
    Qt6::Widgets)
set_target_properties(video_editor_app_tests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)

add_test(NAME app.import_edit_undo COMMAND video_editor_app_tests)
set_tests_properties(app.import_edit_undo PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  TIMEOUT 30)
