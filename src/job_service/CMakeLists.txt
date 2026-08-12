# SPDX-License-Identifier: MPL-2.0

set(protobuf_MODULE_COMPATIBLE TRUE)
find_package(Protobuf CONFIG REQUIRED)

set(VIDEO_EDITOR_JOB_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/proto/job_service.proto")
protobuf_generate_cpp(VIDEO_EDITOR_JOB_PROTO_SOURCES VIDEO_EDITOR_JOB_PROTO_HEADERS
                      "${VIDEO_EDITOR_JOB_PROTO}")

add_library(video_editor_job_service
  ${VIDEO_EDITOR_JOB_PROTO_SOURCES}
  ${VIDEO_EDITOR_JOB_PROTO_HEADERS}
  src/cancellation_registry.cpp
  src/framing.cpp
  src/job_id.cpp)

add_library(video_editor::job_service ALIAS video_editor_job_service)

target_include_directories(
  video_editor_job_service
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_include_directories(
  video_editor_job_service
  SYSTEM PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

if(NOT MSVC)
  set_source_files_properties(${VIDEO_EDITOR_JOB_PROTO_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-sign-conversion")
endif()

target_link_libraries(
  video_editor_job_service
  PUBLIC video_editor::project_options protobuf::libprotobuf
  PRIVATE video_editor::project_warnings)
