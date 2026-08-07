# SPDX-License-Identifier: MPL-2.0

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

add_library(video_editor_asset_service
  src/asset_service.cpp
  src/fingerprint.cpp)

add_library(video_editor::asset_service ALIAS video_editor_asset_service)

target_include_directories(
  video_editor_asset_service
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_asset_service
  PUBLIC video_editor::project_options video_editor::media_codec
  PRIVATE video_editor::project_warnings OpenSSL::Crypto)
