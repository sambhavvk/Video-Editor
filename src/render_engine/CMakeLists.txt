# SPDX-License-Identifier: MPL-2.0

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(video_editor_render_engine LANGUAGES CXX)
  set(_render_engine_standalone ON)

  add_library(video_editor_project_options INTERFACE)
  add_library(video_editor::project_options ALIAS video_editor_project_options)
  add_library(video_editor_project_warnings INTERFACE)
  add_library(video_editor::project_warnings ALIAS video_editor_project_warnings)
  if(MSVC)
    target_compile_options(video_editor_project_warnings INTERFACE /W4 /permissive-)
  else()
    target_compile_options(video_editor_project_warnings INTERFACE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  endif()
  add_subdirectory(../edit_model edit_model)
else()
  set(_render_engine_standalone OFF)
endif()

if(NOT DEFINED VIDEO_EDITOR_LIBPLACEBO_VERSION)
  set(VIDEO_EDITOR_LIBPLACEBO_VERSION "7.360.1")
endif()

option(VIDEO_EDITOR_ENABLE_GPU_RENDERING
       "Enable capability-gated libplacebo D3D11/Vulkan rendering"
       ON)

set(_render_engine_libplacebo_target "")
if(VIDEO_EDITOR_ENABLE_GPU_RENDERING AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  find_package(libplacebo ${VIDEO_EDITOR_LIBPLACEBO_VERSION} EXACT CONFIG QUIET)
  foreach(_candidate libplacebo::libplacebo placebo::placebo libplacebo)
    if(TARGET ${_candidate})
      set(_render_engine_libplacebo_target ${_candidate})
      break()
    endif()
  endforeach()
  find_package(PkgConfig QUIET)
  if(NOT _render_engine_libplacebo_target AND PkgConfig_FOUND)
    pkg_check_modules(RENDER_LIBPLACEBO QUIET IMPORTED_TARGET
                      libplacebo=${VIDEO_EDITOR_LIBPLACEBO_VERSION})
    if(TARGET PkgConfig::RENDER_LIBPLACEBO)
      set(_render_engine_libplacebo_target PkgConfig::RENDER_LIBPLACEBO)
    endif()
  endif()
  if(_render_engine_libplacebo_target AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(VIDEO_EDITOR_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
    if(NOT VIDEO_EDITOR_VULKAN_INCLUDE_DIR)
      message(STATUS
        "libplacebo found without Vulkan headers; compiling truthful GPU-unavailable stub")
      set(_render_engine_libplacebo_target "")
    endif()
  endif()
endif()

add_library(video_editor_render_engine
  src/bitmap_glyphs.cpp
  src/cpu_renderer.cpp
  src/frame.cpp
  src/gpu_backend.cpp
  src/gpu_timeline_renderer.cpp
  src/render_cache.cpp)

add_library(video_editor::render_engine ALIAS video_editor_render_engine)

target_include_directories(
  video_editor_render_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_render_engine
  PUBLIC video_editor::project_options video_editor_edit_model
  PRIVATE video_editor::project_warnings)

if(_render_engine_libplacebo_target)
  target_link_libraries(video_editor_render_engine PRIVATE ${_render_engine_libplacebo_target})
  if(VIDEO_EDITOR_VULKAN_INCLUDE_DIR)
    target_include_directories(video_editor_render_engine SYSTEM PRIVATE
                               ${VIDEO_EDITOR_VULKAN_INCLUDE_DIR})
  endif()
  target_compile_definitions(video_editor_render_engine PRIVATE VIDEO_EDITOR_HAS_LIBPLACEBO=1)
else()
  target_compile_definitions(video_editor_render_engine PRIVATE VIDEO_EDITOR_HAS_LIBPLACEBO=0)
endif()

option(VIDEO_EDITOR_RENDER_ENGINE_BUILD_TESTS
       "Build render-engine unit tests"
       ${_render_engine_standalone})
if(VIDEO_EDITOR_RENDER_ENGINE_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/render_engine"
    "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
