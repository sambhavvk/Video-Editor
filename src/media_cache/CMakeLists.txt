# SPDX-License-Identifier: MPL-2.0

find_package(SQLite3 REQUIRED)

if(TARGET SQLite3::SQLite3)
  set(VIDEO_EDITOR_MEDIA_CACHE_SQLITE_TARGET SQLite3::SQLite3)
else()
  set(VIDEO_EDITOR_MEDIA_CACHE_SQLITE_TARGET SQLite::SQLite3)
endif()

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# Thumbnail and waveform extraction use the pinned FFmpeg ABI for decode and
# (thumbnails) MJPEG encode. The cache store and metadata service are pure
# SQLite/OpenSSL and do not require these.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MEDIA_CACHE_AVFORMAT REQUIRED IMPORTED_TARGET
                  libavformat=${VIDEO_EDITOR_AVFORMAT_VERSION})
pkg_check_modules(MEDIA_CACHE_AVCODEC REQUIRED IMPORTED_TARGET
                  libavcodec=${VIDEO_EDITOR_AVCODEC_VERSION})
pkg_check_modules(MEDIA_CACHE_AVUTIL REQUIRED IMPORTED_TARGET
                  libavutil=${VIDEO_EDITOR_AVUTIL_VERSION})
pkg_check_modules(MEDIA_CACHE_SWSCALE REQUIRED IMPORTED_TARGET
                  libswscale=${VIDEO_EDITOR_SWSCALE_VERSION})
pkg_check_modules(MEDIA_CACHE_SWRESAMPLE REQUIRED IMPORTED_TARGET
                  libswresample=${VIDEO_EDITOR_SWRESAMPLE_VERSION})

add_library(video_editor_media_cache
  src/cache_store.cpp
  src/metadata_service.cpp
  src/thumbnail_service.cpp
  src/waveform_service.cpp)

add_library(video_editor::media_cache ALIAS video_editor_media_cache)

target_include_directories(
  video_editor_media_cache
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  video_editor_media_cache
  PUBLIC
    video_editor::project_options
    video_editor::media_codec
  PRIVATE
    video_editor::project_warnings
    ${VIDEO_EDITOR_MEDIA_CACHE_SQLITE_TARGET}
    OpenSSL::Crypto
    PkgConfig::MEDIA_CACHE_AVFORMAT
    PkgConfig::MEDIA_CACHE_AVCODEC
    PkgConfig::MEDIA_CACHE_AVUTIL
    PkgConfig::MEDIA_CACHE_SWSCALE
    PkgConfig::MEDIA_CACHE_SWRESAMPLE)

set_target_properties(video_editor_media_cache PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
