# SPDX-License-Identifier: MPL-2.0

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(video_editor_interchange LANGUAGES CXX)
  set(_interchange_standalone ON)
  if(NOT DEFINED VIDEO_EDITOR_QT_VERSION)
    set(VIDEO_EDITOR_QT_VERSION "6.11.2")
  endif()
else()
  set(_interchange_standalone OFF)
endif()

find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Core)

if(NOT TARGET VideoEditor::EditModel)
  if(_interchange_standalone)
    set(VIDEO_EDITOR_EDIT_MODEL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(
      "${CMAKE_CURRENT_SOURCE_DIR}/../edit_model"
      "${CMAKE_CURRENT_BINARY_DIR}/edit_model"
    )
  else()
    message(FATAL_ERROR
      "interchange requires the VideoEditor::EditModel target; add edit_model first")
  endif()
endif()

add_library(video_editor_interchange STATIC
  src/otio.cpp
)
add_library(VideoEditor::Interchange ALIAS video_editor_interchange)
add_library(video_editor::interchange ALIAS video_editor_interchange)

target_compile_features(video_editor_interchange PUBLIC cxx_std_20)
target_include_directories(video_editor_interchange
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(video_editor_interchange
  PUBLIC VideoEditor::EditModel
  PRIVATE Qt6::Core
)
set_target_properties(video_editor_interchange PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
  target_compile_options(video_editor_interchange PRIVATE /W4 /permissive-)
else()
  target_compile_options(video_editor_interchange PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
  )
endif()

option(VIDEO_EDITOR_INTERCHANGE_BUILD_TESTS
       "Build interchange unit tests"
       ${_interchange_standalone})
if(VIDEO_EDITOR_INTERCHANGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/interchange"
    "${CMAKE_CURRENT_BINARY_DIR}/tests"
  )
endif()
