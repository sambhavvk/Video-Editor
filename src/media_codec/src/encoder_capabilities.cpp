// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/encoder_capabilities.h"

#include "video_editor/media_codec/runtime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace video_editor::media {
namespace {

struct EncoderSpec {
  std::string_view name;
  DeliveryCodec codec;
};

constexpr std::array<EncoderSpec, 22> kEncoderSpecs{{
    {"h264_nvenc", DeliveryCodec::H264},      {"h264_qsv", DeliveryCodec::H264},
    {"h264_amf", DeliveryCodec::H264},        {"h264_videotoolbox", DeliveryCodec::H264},
    {"libx264", DeliveryCodec::H264},         {"libx264rgb", DeliveryCodec::H264},
    {"hevc_nvenc", DeliveryCodec::Hevc},      {"hevc_qsv", DeliveryCodec::Hevc},
    {"hevc_amf", DeliveryCodec::Hevc},        {"hevc_videotoolbox", DeliveryCodec::Hevc},
    {"libx265", DeliveryCodec::Hevc},         {"av1_nvenc", DeliveryCodec::Av1},
    {"av1_qsv", DeliveryCodec::Av1},          {"av1_amf", DeliveryCodec::Av1},
    {"av1_videotoolbox", DeliveryCodec::Av1}, {"libaom-av1", DeliveryCodec::Av1},
    {"libsvtav1", DeliveryCodec::Av1},        {"aac", DeliveryCodec::Aac},
    {"libfdk_aac", DeliveryCodec::Aac},       {"ffv1", DeliveryCodec::Ffv1},
    {"prores_ks", DeliveryCodec::ProRes},     {"prores_aw", DeliveryCodec::ProRes},
}};

EncoderCategory categorize_encoder(const std::string_view name) noexcept {
  if (name.ends_with("_nvenc")) {
    return EncoderCategory::HardwareNvidia;
  }
  if (name.ends_with("_qsv")) {
    return EncoderCategory::HardwareIntel;
  }
  if (name.ends_with("_amf")) {
    return EncoderCategory::HardwareAmd;
  }
  if (name.ends_with("_videotoolbox")) {
    return EncoderCategory::HardwareApple;
  }
  return EncoderCategory::Software;
}

std::vector<std::string> supported_pixel_formats(const AVCodec& encoder) {
  std::vector<std::string> formats;
  const void* raw_configurations = nullptr;
  int configuration_count = 0;
  const int status = avcodec_get_supported_config(nullptr, &encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                                  &raw_configurations, &configuration_count);
  if (status < 0 || raw_configurations == nullptr || configuration_count <= 0) {
    return formats;
  }

  const auto* pixel_formats = static_cast<const AVPixelFormat*>(raw_configurations);
  formats.reserve(static_cast<std::size_t>(configuration_count));
  for (int index = 0; index < configuration_count; ++index) {
    if (const char* name = av_get_pix_fmt_name(pixel_formats[index]); name != nullptr) {
      formats.emplace_back(name);
    }
  }
  return formats;
}

EncoderCapability probe_encoder(const EncoderSpec& spec) {
  EncoderCapability capability{
      .encoder_name = std::string(spec.name),
      .codec = spec.codec,
      .category = EncoderCategory::Software,
      .available = false,
      .hardware = false,
      .supported_pixel_formats = {},
      .max_profile = {},
      .max_level = {},
  };

  const AVCodec* encoder = avcodec_find_encoder_by_name(capability.encoder_name.c_str());
  if (encoder == nullptr) {
    return capability;
  }

  capability.available = true;
  capability.category = categorize_encoder(spec.name);
  capability.hardware = capability.category != EncoderCategory::Software;
  capability.supported_pixel_formats = supported_pixel_formats(*encoder);
  return capability;
}

std::string codec_name(const DeliveryCodec codec) {
  switch (codec) {
  case DeliveryCodec::H264:
    return "H.264";
  case DeliveryCodec::Aac:
    return "AAC";
  case DeliveryCodec::Hevc:
    return "HEVC";
  case DeliveryCodec::Av1:
    return "AV1";
  case DeliveryCodec::Ffv1:
    return "FFV1";
  case DeliveryCodec::ProRes:
    return "ProRes";
  }
  return "Unknown";
}

} // namespace

EncoderCapabilityMatrix probe_encoder_capabilities() {
  EncoderCapabilityMatrix matrix;
  try {
    matrix.lgpl_compatible_runtime = runtime_info().lgpl_compatible_configuration;
#ifdef VIDEO_EDITOR_H264_DELIVERY_APPROVED
    matrix.h264_delivery_approved = true;
#endif
#ifdef VIDEO_EDITOR_AAC_DELIVERY_APPROVED
    matrix.aac_delivery_approved = true;
#endif

    matrix.encoders.reserve(kEncoderSpecs.size());
    for (const EncoderSpec& spec : kEncoderSpecs) {
      try {
        matrix.encoders.push_back(probe_encoder(spec));
      } catch (...) {
        EncoderCapability unavailable{
            .encoder_name = std::string(spec.name),
            .codec = spec.codec,
            .category = EncoderCategory::Software,
            .available = false,
            .hardware = false,
            .supported_pixel_formats = {},
            .max_profile = {},
            .max_level = {},
        };
        matrix.encoders.push_back(std::move(unavailable));
      }
    }
  } catch (...) {
    // Capability detection is diagnostic infrastructure and must not affect export startup.
  }
  return matrix;
}

std::optional<EncoderCapability> best_encoder_for(const EncoderCapabilityMatrix& matrix,
                                                  const DeliveryCodec codec) {
  const EncoderCapability* best = nullptr;
  for (const EncoderCapability& capability : matrix.encoders) {
    if (capability.codec != codec || !capability.available) {
      continue;
    }
    if (best == nullptr || (capability.hardware && !best->hardware)) {
      best = &capability;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  return *best;
}

bool has_hardware_encoder(const EncoderCapabilityMatrix& matrix, const DeliveryCodec codec) {
  return std::any_of(
      matrix.encoders.begin(), matrix.encoders.end(), [codec](const auto& capability) {
        return capability.codec == codec && capability.available && capability.hardware;
      });
}

std::string format_capability_summary(const EncoderCapabilityMatrix& matrix) {
  std::string summary = "Encoder Capability Matrix\n";
  summary += "LGPL compatible runtime: ";
  summary += matrix.lgpl_compatible_runtime ? "yes\n" : "no\n";
  summary += "H.264 delivery approved: ";
  summary += matrix.h264_delivery_approved ? "yes\n" : "no\n";
  summary += "AAC delivery approved: ";
  summary += matrix.aac_delivery_approved ? "yes\n\n" : "no\n\n";

  for (const DeliveryCodec codec :
       {DeliveryCodec::H264, DeliveryCodec::Aac, DeliveryCodec::Hevc, DeliveryCodec::Av1,
        DeliveryCodec::Ffv1, DeliveryCodec::ProRes}) {
    summary += codec_name(codec) + ":\n";
    for (const EncoderCapability& capability : matrix.encoders) {
      if (capability.codec != codec) {
        continue;
      }
      summary += "  " + capability.encoder_name + ": ";
      summary += capability.available ? "available\n" : "not available\n";
    }
  }
  return summary;
}

} // namespace video_editor::media
