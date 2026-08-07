# SPDX-License-Identifier: MPL-2.0

find_program(VIDEO_EDITOR_TEST_FFMPEG NAMES ffmpeg)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PROXY_TEST_AVFORMAT REQUIRED IMPORTED_TARGET
                  libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(PROXY_TEST_AVCODEC REQUIRED IMPORTED_TARGET
                  libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(PROXY_TEST_AVUTIL REQUIRED IMPORTED_TARGET
                  libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})

add_executable(video_editor_proxy_service_tests
  proxy_service_test.cpp)

target_link_libraries(
  video_editor_proxy_service_tests
  PRIVATE
    VideoEditor::ProxyService
    video_editor::media_codec
    video_editor::project_warnings
    PkgConfig::PROXY_TEST_AVFORMAT
    PkgConfig::PROXY_TEST_AVCODEC
    PkgConfig::PROXY_TEST_AVUTIL
    GTest::gtest_main)

if(VIDEO_EDITOR_TEST_FFMPEG)
  target_compile_definitions(
    video_editor_proxy_service_tests
    PRIVATE VIDEO_EDITOR_TEST_FFMPEG="${VIDEO_EDITOR_TEST_FFMPEG}")
endif()

include(GoogleTest)
gtest_discover_tests(video_editor_proxy_service_tests)
