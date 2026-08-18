# SPDX-License-Identifier: MPL-2.0

find_program(VIDEO_EDITOR_TEST_FFMPEG NAMES ffmpeg)

find_package(PkgConfig REQUIRED)
pkg_check_modules(EXPORT_TEST_AVFORMAT REQUIRED IMPORTED_TARGET
                  libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(EXPORT_TEST_AVCODEC REQUIRED IMPORTED_TARGET
                  libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(EXPORT_TEST_AVUTIL REQUIRED IMPORTED_TARGET
                  libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})

add_executable(video_editor_media_codec_tests
  probe_test.cpp)

target_link_libraries(
  video_editor_media_codec_tests
  PRIVATE
    video_editor::media_codec
    video_editor::project_warnings
    GTest::gtest_main
    PkgConfig::EXPORT_TEST_AVFORMAT
    PkgConfig::EXPORT_TEST_AVCODEC
    PkgConfig::EXPORT_TEST_AVUTIL)

if(VIDEO_EDITOR_TEST_FFMPEG)
  target_compile_definitions(
    video_editor_media_codec_tests
    PRIVATE VIDEO_EDITOR_TEST_FFMPEG="${VIDEO_EDITOR_TEST_FFMPEG}")
endif()

add_executable(video_editor_encoder_capabilities_tests
  encoder_capabilities_test.cpp)

target_link_libraries(
  video_editor_encoder_capabilities_tests
  PRIVATE
    video_editor::media_codec
    video_editor::project_warnings
    GTest::gtest_main
    PkgConfig::EXPORT_TEST_AVFORMAT
    PkgConfig::EXPORT_TEST_AVCODEC
    PkgConfig::EXPORT_TEST_AVUTIL)

include(GoogleTest)
gtest_discover_tests(video_editor_media_codec_tests)
gtest_discover_tests(video_editor_encoder_capabilities_tests)
