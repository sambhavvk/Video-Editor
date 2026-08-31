# SPDX-License-Identifier: MPL-2.0

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.30)
  project(video_editor_caption_service LANGUAGES CXX)
  set(_caption_service_standalone ON)
else()
  set(_caption_service_standalone OFF)
endif()

if(NOT TARGET video_editor_edit_model)
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../edit_model"
    "${CMAKE_CURRENT_BINARY_DIR}/edit_model")
endif()

add_library(video_editor_caption_service STATIC
  src/conversion.cpp
  src/internal.cpp
  src/parser.cpp
  src/proposal.cpp
  src/reflow.cpp
  src/search.cpp
  src/serializer.cpp)
if(NOT _caption_service_standalone)
  target_sources(video_editor_caption_service PRIVATE src/embedded_extract.cpp)
endif()
add_library(VideoEditor::CaptionService ALIAS video_editor_caption_service)

target_compile_features(video_editor_caption_service PUBLIC cxx_std_20)
target_include_directories(video_editor_caption_service
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(video_editor_caption_service PUBLIC VideoEditor::EditModel)
if(NOT _caption_service_standalone)
  target_link_libraries(video_editor_caption_service PUBLIC video_editor::media_codec)
endif()
set_target_properties(video_editor_caption_service PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)

if(TARGET video_editor::project_options)
  target_link_libraries(video_editor_caption_service
    PUBLIC video_editor::project_options
    PRIVATE video_editor::project_warnings)
elseif(MSVC)
  target_compile_options(video_editor_caption_service PRIVATE /W4 /permissive-)
else()
  target_compile_options(video_editor_caption_service PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
endif()

option(VIDEO_EDITOR_CAPTION_SERVICE_BUILD_TESTS
       "Build caption-service unit tests"
       ${_caption_service_standalone})
if(VIDEO_EDITOR_CAPTION_SERVICE_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/caption_service"
    "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
