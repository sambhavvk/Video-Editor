# SPDX-License-Identifier: MPL-2.0

find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Test Widgets)
find_program(VIDEO_EDITOR_APP_TEST_FFMPEG NAMES ffmpeg REQUIRED)

add_executable(video_editor_app_tests editor_controller_test.cpp)
target_compile_features(video_editor_app_tests PRIVATE cxx_std_20)
target_link_libraries(video_editor_app_tests
  PRIVATE
    video_editor_app_controller
    video_editor::project_warnings
    Qt6::Test
    Qt6::Widgets)
target_compile_definitions(video_editor_app_tests
  PRIVATE VIDEO_EDITOR_APP_TEST_FFMPEG="${VIDEO_EDITOR_APP_TEST_FFMPEG}")
set_target_properties(video_editor_app_tests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)

add_test(NAME app.import_edit_undo COMMAND video_editor_app_tests)
if(VIDEO_EDITOR_ENABLE_SANITIZERS)
  set(VIDEO_EDITOR_APP_TEST_TIMEOUT 180)
else()
  set(VIDEO_EDITOR_APP_TEST_TIMEOUT 30)
endif()
set_tests_properties(app.import_edit_undo PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  TIMEOUT ${VIDEO_EDITOR_APP_TEST_TIMEOUT})
