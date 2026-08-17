# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

# Versions accepted by the source contracts. Official distribution builds use
# reproducible binary bundles of these versions; local developer builds may use
# ABI-identical system packages and must run the dependency audit.
set(VIDEO_EDITOR_QT_VERSION "6.11.1")
set(VIDEO_EDITOR_FFMPEG_VERSION "9.0.1")
set(VIDEO_EDITOR_AVFORMAT_VERSION "63.1.101")
set(VIDEO_EDITOR_AVCODEC_VERSION "63.1.101")
set(VIDEO_EDITOR_AVUTIL_VERSION "61.1.101")
set(VIDEO_EDITOR_SWRESAMPLE_VERSION "7.1.101")
set(VIDEO_EDITOR_SWSCALE_VERSION "10.1.101")
set(VIDEO_EDITOR_LIBPLACEBO_VERSION "7.360.1")
set(VIDEO_EDITOR_MINIAUDIO_VERSION "0.11.25")
set(VIDEO_EDITOR_SQLITE_MIN_VERSION "3.45")
set(VIDEO_EDITOR_ABSEIL_VERSION "20250512.1")
set(VIDEO_EDITOR_PROTOBUF_VERSION "35.1")
set(VIDEO_EDITOR_OPENSSL_MIN_VERSION "3.0")
set(VIDEO_EDITOR_EBUR128_VERSION "1.2.6")

# Optional transcription backend and its on-demand model contract. The worker
# remains buildable without whisper.cpp; enabling the backend requires a local
# build of this exact upstream release.
set(VIDEO_EDITOR_WHISPER_CPP_VERSION "1.9.2")
set(VIDEO_EDITOR_WHISPER_CPP_COMMIT "306c88f4d1286aec1bf96e544632897886af5501")
set(VIDEO_EDITOR_WHISPER_MODEL_ID "base")
set(VIDEO_EDITOR_WHISPER_MODEL_FILENAME "ggml-base.bin")
set(VIDEO_EDITOR_WHISPER_MODEL_BYTES "147951465")
set(VIDEO_EDITOR_WHISPER_MODEL_DIGEST_ALGORITHM "sha1")
set(VIDEO_EDITOR_WHISPER_MODEL_DIGEST "465707469ff3a37a2b9b8d8f89f2f99de7299dac")
set(VIDEO_EDITOR_WHISPER_MODEL_URL "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin")
