# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

# Versions accepted by the source contracts. Official distribution builds use
# reproducible binary bundles of these versions; local developer builds may use
# ABI-identical system packages and must run the dependency audit.
set(VIDEO_EDITOR_QT_VERSION "6.11.1")
set(VIDEO_EDITOR_FFMPEG_VERSION "8.1.2")
set(VIDEO_EDITOR_AVFORMAT_VERSION "62.12.102")
set(VIDEO_EDITOR_AVCODEC_VERSION "62.28.102")
set(VIDEO_EDITOR_AVUTIL_VERSION "60.26.102")
set(VIDEO_EDITOR_SWRESAMPLE_VERSION "6.3.102")
set(VIDEO_EDITOR_SWSCALE_VERSION "9.5.102")
set(VIDEO_EDITOR_LIBPLACEBO_VERSION "7.360.1")
set(VIDEO_EDITOR_SQLITE_MIN_VERSION "3.45")
set(VIDEO_EDITOR_ABSEIL_VERSION "20250512.1")
set(VIDEO_EDITOR_PROTOBUF_VERSION "35.1")
set(VIDEO_EDITOR_OPENSSL_MIN_VERSION "3.0")
set(VIDEO_EDITOR_EBUR128_VERSION "1.2.6")
