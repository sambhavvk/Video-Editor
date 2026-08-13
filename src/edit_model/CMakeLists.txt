# SPDX-License-Identifier: MPL-2.0

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(video_editor_edit_model LANGUAGES CXX)
  set(_edit_model_standalone ON)
else()
  set(_edit_model_standalone OFF)
endif()

# The exact-time implementation relies on the standard Clang/GCC 128-bit
# integer extension. clang-cl supports it on Windows; native MSVC does not.
# Fail during configuration with an actionable diagnostic instead of deep in
# the rational arithmetic implementation.
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "video_editor_edit_model requires 128-bit integer intermediates. "
    "Configure the Windows build with clang-cl; native MSVC is not supported."
  )
endif()

add_library(video_editor_edit_model STATIC
  src/effect_evaluator.cpp
  src/entity_id.cpp
  src/model.cpp
  src/snapping.cpp
  src/time.cpp
  src/timeline_editor.cpp
)
add_library(VideoEditor::EditModel ALIAS video_editor_edit_model)

target_compile_features(video_editor_edit_model PUBLIC cxx_std_20)
target_include_directories(video_editor_edit_model
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(video_editor_edit_model PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
  target_compile_options(video_editor_edit_model PRIVATE /W4 /permissive-)
else()
  target_compile_options(video_editor_edit_model PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
  )
endif()

option(VIDEO_EDITOR_EDIT_MODEL_BUILD_TESTS
       "Build edit-model unit tests"
       ${_edit_model_standalone})
if(VIDEO_EDITOR_EDIT_MODEL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/edit_model"
    "${CMAKE_CURRENT_BINARY_DIR}/tests"
  )
endif()
