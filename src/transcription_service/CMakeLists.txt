# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

option(VIDEO_EDITOR_ENABLE_WHISPER_CPP "Enable the optional whisper.cpp backend" OFF)

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
         $<INSTALL_INTERFACE:include>)
target_link_libraries(video_editor_transcription_service
  PUBLIC video_editor::project_options
         video_editor::edit_model
         video_editor::job_service
  PRIVATE video_editor::project_warnings
          OpenSSL::Crypto)

pkg_check_modules(TRANSCRIPTION_AVFORMAT REQUIRED IMPORTED_TARGET
                  libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(TRANSCRIPTION_AVCODEC REQUIRED IMPORTED_TARGET
                  libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(TRANSCRIPTION_AVUTIL REQUIRED IMPORTED_TARGET
                  libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})
pkg_check_modules(TRANSCRIPTION_SWRESAMPLE REQUIRED IMPORTED_TARGET
                  libswresample=${VIDEO_EDITOR_SWRESAMPLE_VERSION})
target_link_libraries(video_editor_transcription_service PRIVATE
  PkgConfig::TRANSCRIPTION_AVFORMAT PkgConfig::TRANSCRIPTION_AVCODEC
  PkgConfig::TRANSCRIPTION_AVUTIL PkgConfig::TRANSCRIPTION_SWRESAMPLE)

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
endif()
