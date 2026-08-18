# SPDX-License-Identifier: MPL-2.0

find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Test Widgets)
find_program(VIDEO_EDITOR_APP_TEST_FFMPEG NAMES ffmpeg REQUIRED)

add_executable(video_editor_app_tests editor_controller_test.cpp)
target_compile_features(video_editor_app_tests PRIVATE cxx_std_20)
target_link_libraries(video_editor_app_tests
  PRIVATE
    video_editor_app_controller
    video_editor::media_cache
    video_editor::project_warnings
    Qt6::Test
    Qt6::Widgets)
target_compile_definitions(video_editor_app_tests
  PRIVATE VIDEO_EDITOR_APP_TEST_FFMPEG="${VIDEO_EDITOR_APP_TEST_FFMPEG}")
set_target_properties(video_editor_app_tests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)

add_test(NAME app.import_edit_undo COMMAND video_editor_app_tests)
if(VIDEO_EDITOR_ENABLE_SANITIZERS)
  set(VIDEO_EDITOR_APP_TEST_TIMEOUT 300)
else()
  set(VIDEO_EDITOR_APP_TEST_TIMEOUT 60)
endif()
set_tests_properties(app.import_edit_undo PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  TIMEOUT ${VIDEO_EDITOR_APP_TEST_TIMEOUT})

add_executable(video_editor_session_event_log_tests session_event_log_test.cpp)
target_compile_features(video_editor_session_event_log_tests PRIVATE cxx_std_20)
target_link_libraries(video_editor_session_event_log_tests
  PRIVATE
    video_editor_app_controller
    video_editor::project_warnings
    Qt6::Test
    Qt6::Widgets)
set_target_properties(video_editor_session_event_log_tests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
add_test(NAME app.session_event_log COMMAND video_editor_session_event_log_tests)
set_tests_properties(app.session_event_log PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  TIMEOUT ${VIDEO_EDITOR_APP_TEST_TIMEOUT})

add_executable(video_editor_app_fault_injection_tests fault_injection_test.cpp)
target_compile_features(video_editor_app_fault_injection_tests PRIVATE cxx_std_20)
target_link_libraries(video_editor_app_fault_injection_tests
  PRIVATE
    video_editor_app_controller
    video_editor::render_engine
    video_editor::project_warnings
    Qt6::Test
    Qt6::Widgets)
set_target_properties(video_editor_app_fault_injection_tests PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
add_test(NAME app.fault_injection COMMAND video_editor_app_fault_injection_tests)
set_tests_properties(app.fault_injection PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  TIMEOUT ${VIDEO_EDITOR_APP_TEST_TIMEOUT})

if(TARGET video_editor_worker_host)
  add_dependencies(video_editor_app_tests video_editor_worker_host)
  target_compile_definitions(video_editor_app_tests PRIVATE
    VIDEO_EDITOR_APP_TEST_WORKER_HOST="$<TARGET_FILE:video_editor_worker_host>")
endif()
set_tests_properties(app.import_edit_undo PROPERTIES TIMEOUT 180)
