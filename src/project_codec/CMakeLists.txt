# SPDX-License-Identifier: MPL-2.0

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(video_editor_project_codec LANGUAGES CXX)
  set(_project_codec_standalone ON)
else()
  set(_project_codec_standalone OFF)
endif()

set(protobuf_MODULE_COMPATIBLE TRUE)
find_package(Protobuf CONFIG REQUIRED)

if(NOT TARGET VideoEditor::EditModel)
  if(_project_codec_standalone)
    set(VIDEO_EDITOR_EDIT_MODEL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(
      "${CMAKE_CURRENT_SOURCE_DIR}/../edit_model"
      "${CMAKE_CURRENT_BINARY_DIR}/edit_model"
    )
  else()
    message(FATAL_ERROR
      "project_codec requires the VideoEditor::EditModel target; add edit_model first")
  endif()
endif()

set(VIDEO_EDITOR_PROJECT_SNAPSHOT_PROTO
    "${CMAKE_CURRENT_SOURCE_DIR}/proto/project_snapshot.proto")
protobuf_generate_cpp(
  VIDEO_EDITOR_PROJECT_PROTO_SOURCES
  VIDEO_EDITOR_PROJECT_PROTO_HEADERS
  "${VIDEO_EDITOR_PROJECT_SNAPSHOT_PROTO}"
)

add_library(video_editor_project_codec STATIC
  ${VIDEO_EDITOR_PROJECT_PROTO_SOURCES}
  ${VIDEO_EDITOR_PROJECT_PROTO_HEADERS}
  src/project_codec.cpp
)
add_library(VideoEditor::ProjectCodec ALIAS video_editor_project_codec)
add_library(video_editor::project_codec ALIAS video_editor_project_codec)

target_compile_features(video_editor_project_codec PUBLIC cxx_std_20)
target_include_directories(video_editor_project_codec
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(video_editor_project_codec SYSTEM
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
target_link_libraries(video_editor_project_codec
  PUBLIC VideoEditor::EditModel
  PRIVATE protobuf::libprotobuf
)
set_target_properties(video_editor_project_codec PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
  target_compile_options(video_editor_project_codec PRIVATE /W4 /permissive-)
else()
  target_compile_options(video_editor_project_codec PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
  )
  set_source_files_properties(${VIDEO_EDITOR_PROJECT_PROTO_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-sign-conversion")
endif()

option(VIDEO_EDITOR_PROJECT_CODEC_BUILD_TESTS
       "Build project-codec unit tests"
       ${_project_codec_standalone})
if(VIDEO_EDITOR_PROJECT_CODEC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/project_codec"
    "${CMAKE_CURRENT_BINARY_DIR}/tests"
  )
endif()
