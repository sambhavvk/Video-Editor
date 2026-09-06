# SPDX-License-Identifier: MPL-2.0

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(video_editor_agent_protocol LANGUAGES CXX)
  set(_agent_protocol_standalone ON)
  if(NOT DEFINED VIDEO_EDITOR_QT_VERSION)
    set(VIDEO_EDITOR_QT_VERSION "6.11.2")
  endif()
else()
  set(_agent_protocol_standalone OFF)
endif()

find_package(Qt6 ${VIDEO_EDITOR_QT_VERSION} EXACT REQUIRED COMPONENTS Core)

if(NOT TARGET VideoEditor::EditModel)
  if(_agent_protocol_standalone)
    set(VIDEO_EDITOR_EDIT_MODEL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(
      "${CMAKE_CURRENT_SOURCE_DIR}/../edit_model"
      "${CMAKE_CURRENT_BINARY_DIR}/edit_model"
    )
  else()
    message(FATAL_ERROR
      "agent_protocol requires the VideoEditor::EditModel target; add edit_model first")
  endif()
endif()

add_library(video_editor_agent_protocol STATIC
  src/edit_command_json.cpp
)
add_library(VideoEditor::AgentProtocol ALIAS video_editor_agent_protocol)
add_library(video_editor::agent_protocol ALIAS video_editor_agent_protocol)

target_compile_features(video_editor_agent_protocol PUBLIC cxx_std_20)
target_include_directories(video_editor_agent_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(video_editor_agent_protocol
  PUBLIC VideoEditor::EditModel
  PRIVATE Qt6::Core
)
set_target_properties(video_editor_agent_protocol PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
  target_compile_options(video_editor_agent_protocol PRIVATE /W4 /permissive-)
else()
  target_compile_options(video_editor_agent_protocol PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
  )
endif()

option(VIDEO_EDITOR_AGENT_PROTOCOL_BUILD_TESTS
       "Build agent_protocol unit tests"
       ${_agent_protocol_standalone})
if(VIDEO_EDITOR_AGENT_PROTOCOL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/agent_protocol"
    "${CMAKE_CURRENT_BINARY_DIR}/tests"
  )
endif()
