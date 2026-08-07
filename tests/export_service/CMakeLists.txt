# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)
pkg_check_modules(EXPORT_TEST_AVFORMAT REQUIRED IMPORTED_TARGET
                  libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(EXPORT_TEST_AVCODEC REQUIRED IMPORTED_TARGET
                  libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(EXPORT_TEST_AVUTIL REQUIRED IMPORTED_TARGET
                  libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})

add_executable(video_editor_export_service_tests
  export_service_test.cpp)

target_link_libraries(
  video_editor_export_service_tests
  PRIVATE
    VideoEditor::ExportService
    video_editor::project_warnings
    GTest::gtest_main
    PkgConfig::EXPORT_TEST_AVFORMAT
    PkgConfig::EXPORT_TEST_AVCODEC
    PkgConfig::EXPORT_TEST_AVUTIL)

include(GoogleTest)
gtest_discover_tests(video_editor_export_service_tests)
