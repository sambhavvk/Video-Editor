# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

configure_file(
  include/video_editor/transcription_service/model_manifest.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/video_editor/transcription_service/model_manifest.h
  @ONLY)

option(VIDEO_EDITOR_ENABLE_WHISPER_CPP "Enable the optional whisper.cpp backend" OFF)
option(VIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED
       "Assert linked whisper.cpp was built with Vulkan (provenance only; runtime use is not reported)"
       OFF)

if(NOT VIDEO_EDITOR_WHISPER_CPP_VERSION STREQUAL "1.9.2" OR
   NOT VIDEO_EDITOR_WHISPER_CPP_COMMIT STREQUAL
       "306c88f4d1286aec1bf96e544632897886af5501")
  message(FATAL_ERROR
    "The transcription adapter is pinned to whisper.cpp v1.9.2 at 306c88f4d1286aec1bf96e544632897886af5501")
endif()

set(VIDEO_EDITOR_TRANSCRIPTION_WHISPER_INCLUDE_DIR "" CACHE PATH
    "Directory containing the pinned whisper.cpp whisper.h")
set(VIDEO_EDITOR_TRANSCRIPTION_WHISPER_LIBRARY "" CACHE FILEPATH
    "whisper.cpp library for the optional transcription backend")

add_library(video_editor_transcription_service
  src/transcription_service.cpp
  src/audio_decoder.cpp
  src/model_manager.cpp
  src/whisper_backend.cpp)
add_library(video_editor::transcription_service ALIAS video_editor_transcription_service)

target_include_directories(video_editor_transcription_service
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(video_editor_transcription_service
  PUBLIC video_editor::project_options
         VideoEditor::EditModel
         video_editor::job_service
  PRIVATE video_editor::project_warnings
          video_editor::media_codec
          OpenSSL::Crypto)

target_link_libraries(video_editor_transcription_service PRIVATE
  PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWRESAMPLE)

if(VIDEO_EDITOR_ENABLE_WHISPER_CPP)
  if(NOT VIDEO_EDITOR_TRANSCRIPTION_WHISPER_INCLUDE_DIR OR
     NOT VIDEO_EDITOR_TRANSCRIPTION_WHISPER_LIBRARY)
    message(FATAL_ERROR
      "VIDEO_EDITOR_ENABLE_WHISPER_CPP requires the pinned whisper.cpp include and library")
  endif()
  target_include_directories(video_editor_transcription_service PRIVATE
    ${VIDEO_EDITOR_TRANSCRIPTION_WHISPER_INCLUDE_DIR})
  target_link_libraries(video_editor_transcription_service PRIVATE
    ${VIDEO_EDITOR_TRANSCRIPTION_WHISPER_LIBRARY})
  target_compile_definitions(video_editor_transcription_service PRIVATE
    VIDEO_EDITOR_HAVE_WHISPER_CPP=1)
  if(VIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED)
    target_compile_definitions(video_editor_transcription_service PRIVATE
      VIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED=1)
  endif()
endif()
