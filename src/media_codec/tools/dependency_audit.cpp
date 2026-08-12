// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/runtime.h"

#include "video_editor/media_codec/dependency_versions.h"

#include <iostream>
#include <string_view>

int main(const int argument_count, char** arguments) {
  const bool official = argument_count > 1 && std::string_view(arguments[1]) == "--official";
  const video_editor::media::RuntimeInfo info = video_editor::media::runtime_info();
  std::cout << "{\n"
            << "  \"avformat\": \"" << info.avformat.major << '.' << info.avformat.minor << '.'
            << info.avformat.patch << "\",\n"
            << "  \"avcodec\": \"" << info.avcodec.major << '.' << info.avcodec.minor << '.'
            << info.avcodec.patch << "\",\n"
            << "  \"expected_abi\": " << (info.expected_abi ? "true" : "false") << ",\n"
            << "  \"lgpl_compatible_configuration\": "
            << (info.lgpl_compatible_configuration ? "true" : "false") << "\n"
            << "}\n";

  if (!info.expected_abi) {
    using namespace video_editor::media::dependency_versions;
    std::cerr << "FFmpeg ABI does not match the pinned " << kFfmpegRelease
              << " contract (libavformat " << kAvformatMajor << '.' << kAvformatMinor << '.'
              << kAvformatPatch << ", libavcodec " << kAvcodecMajor << '.' << kAvcodecMinor << '.'
              << kAvcodecPatch << ").\n";
    return 2;
  }
  if (official && !info.lgpl_compatible_configuration) {
    std::cerr << "Official distribution rejected: FFmpeg enables GPL or nonfree components.\n";
    return 3;
  }
  return 0;
}
