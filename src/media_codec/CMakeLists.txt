# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)

# Legal gate flags for delivery codecs
# These default to OFF and are flipped by release management after codec/patent review
option(VIDEO_EDITOR_H264_DELIVERY_APPROVED "H.264 delivery legally approved for distribution" OFF)
option(VIDEO_EDITOR_AAC_DELIVERY_APPROVED "AAC delivery legally approved for distribution" OFF)

function(video_editor_split_dependency_version version prefix)
  if(NOT "${version}" MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "${prefix} must be an exact major.minor.patch version: ${version}")
  endif()
  set(${prefix}_MAJOR "${CMAKE_MATCH_1}" PARENT_SCOPE)
  set(${prefix}_MINOR "${CMAKE_MATCH_2}" PARENT_SCOPE)
  set(${prefix}_PATCH "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()

video_editor_split_dependency_version(
  "${VIDEO_EDITOR_AVFORMAT_VERSION}" VIDEO_EDITOR_EXPECTED_AVFORMAT)
video_editor_split_dependency_version(
  "${VIDEO_EDITOR_AVCODEC_VERSION}" VIDEO_EDITOR_EXPECTED_AVCODEC)
video_editor_split_dependency_version(
  "${VIDEO_EDITOR_AVUTIL_VERSION}" VIDEO_EDITOR_EXPECTED_AVUTIL)
video_editor_split_dependency_version(
  "${VIDEO_EDITOR_SWRESAMPLE_VERSION}" VIDEO_EDITOR_EXPECTED_SWRESAMPLE)
video_editor_split_dependency_version(
  "${VIDEO_EDITOR_SWSCALE_VERSION}" VIDEO_EDITOR_EXPECTED_SWSCALE)

configure_file(
  include/video_editor/media_codec/dependency_versions.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/video_editor/media_codec/dependency_versions.h
  @ONLY)

pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample=${VIDEO_EDITOR_SWRESAMPLE_VERSION})
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale=${VIDEO_EDITOR_SWSCALE_VERSION})

# Make the canonical FFmpeg imported targets reusable by media consumers
# (including the optional transcription service) without repeating package
# discovery or allowing a second ABI contract to drift.
set_property(TARGET PkgConfig::AVFORMAT PROPERTY IMPORTED_GLOBAL TRUE)
set_property(TARGET PkgConfig::AVCODEC PROPERTY IMPORTED_GLOBAL TRUE)
set_property(TARGET PkgConfig::AVUTIL PROPERTY IMPORTED_GLOBAL TRUE)
set_property(TARGET PkgConfig::SWRESAMPLE PROPERTY IMPORTED_GLOBAL TRUE)
set_property(TARGET PkgConfig::SWSCALE PROPERTY IMPORTED_GLOBAL TRUE)

add_library(video_editor_media_codec
  src/probe.cpp
  src/runtime.cpp
  src/encoder_capabilities.cpp)

add_library(video_editor::media_codec ALIAS video_editor_media_codec)

target_include_directories(
  video_editor_media_codec
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_media_codec
  PUBLIC video_editor::project_options
  PRIVATE
    video_editor::project_warnings
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWRESAMPLE
    PkgConfig::SWSCALE)

# Legal gate compile definitions (only set when ON)
if(VIDEO_EDITOR_H264_DELIVERY_APPROVED)
  target_compile_definitions(video_editor_media_codec PRIVATE VIDEO_EDITOR_H264_DELIVERY_APPROVED=1)
endif()
if(VIDEO_EDITOR_AAC_DELIVERY_APPROVED)
  target_compile_definitions(video_editor_media_codec PRIVATE VIDEO_EDITOR_AAC_DELIVERY_APPROVED=1)
endif()

set_target_properties(video_editor_media_codec PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

add_executable(video_editor_dependency_audit tools/dependency_audit.cpp)
target_link_libraries(video_editor_dependency_audit PRIVATE video_editor::media_codec)
