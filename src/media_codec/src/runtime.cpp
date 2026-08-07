// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/runtime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <string_view>

namespace video_editor::media {
namespace {

LibraryVersion unpack_version(const unsigned version) {
  return {
      .major = version >> 16U,
      .minor = (version >> 8U) & 0xFFU,
      .patch = version & 0xFFU,
  };
}

bool has_option(const std::string_view configuration, const std::string_view option) {
  auto lower = std::string(configuration);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return lower.find(option) != std::string::npos;
}

} // namespace

RuntimeInfo runtime_info() {
  RuntimeInfo info{
      .avformat = unpack_version(avformat_version()),
      .avcodec = unpack_version(avcodec_version()),
      .avutil = unpack_version(avutil_version()),
      .swresample = unpack_version(swresample_version()),
      .swscale = unpack_version(swscale_version()),
      .configuration = avcodec_configuration(),
      .license = avcodec_license(),
  };

  info.expected_abi = info.avformat.major == 62U && info.avcodec.major == 62U &&
                      info.avutil.major == 60U && info.swresample.major == 6U &&
                      info.swscale.major == 9U;
  info.lgpl_compatible_configuration =
      !has_option(info.configuration, "--enable-gpl") &&
      !has_option(info.configuration, "--enable-nonfree") &&
      info.license.find("LGPL") != std::string::npos;
  return info;
}

} // namespace video_editor::media

