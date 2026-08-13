// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/encoder_capabilities.h"

#include "video_editor/media_codec/runtime.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
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

AVHWDeviceType hardware_device_type(const std::string_view name) noexcept {
  if (name == "vp9_vaapi") {
    return AV_HWDEVICE_TYPE_VAAPI;
  }
  if (name == "vp9_qsv") {
    return AV_HWDEVICE_TYPE_QSV;
  }
  return AV_HWDEVICE_TYPE_NONE;
}

AVPixelFormat hardware_pixel_format(const std::string_view name) noexcept {
  if (name == "vp9_vaapi") {
    return AV_PIX_FMT_VAAPI;
  }
  if (name == "vp9_qsv") {
    return AV_PIX_FMT_QSV;
  }
  return AV_PIX_FMT_NONE;
}

constexpr std::array<EncoderSpec, 29> kEncoderSpecs{{
    {"h264_nvenc", DeliveryCodec::H264},
    {"h264_qsv", DeliveryCodec::H264},
    {"h264_amf", DeliveryCodec::H264},
    {"h264_videotoolbox", DeliveryCodec::H264},
    {"libx264", DeliveryCodec::H264},
    {"libx264rgb", DeliveryCodec::H264},
    {"hevc_nvenc", DeliveryCodec::Hevc},
    {"hevc_qsv", DeliveryCodec::Hevc},
    {"hevc_amf", DeliveryCodec::Hevc},
    {"hevc_videotoolbox", DeliveryCodec::Hevc},
    {"libx265", DeliveryCodec::Hevc},
    {"av1_nvenc", DeliveryCodec::Av1},
    {"av1_qsv", DeliveryCodec::Av1},
    {"av1_amf", DeliveryCodec::Av1},
    {"av1_videotoolbox", DeliveryCodec::Av1},
    {"libaom-av1", DeliveryCodec::Av1},
    {"libsvtav1", DeliveryCodec::Av1},
    {"av1_vaapi", DeliveryCodec::Av1},
    {"av1_vulkan", DeliveryCodec::Av1},
    {"aac", DeliveryCodec::Aac},
    {"libfdk_aac", DeliveryCodec::Aac},
    {"libvpx-vp9", DeliveryCodec::Vp9},
    {"vp9_vaapi", DeliveryCodec::Vp9},
    {"vp9_qsv", DeliveryCodec::Vp9},
    {"libopus", DeliveryCodec::Opus},
    {"opus", DeliveryCodec::Opus},
    {"ffv1", DeliveryCodec::Ffv1},
    {"prores_ks", DeliveryCodec::ProRes},
    {"prores_aw", DeliveryCodec::ProRes},
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
  if (name.ends_with("_vaapi") || name.ends_with("_vulkan")) {
    return EncoderCategory::HardwareOther;
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

EncoderCapability probe_encoder(const EncoderSpec& spec, const bool probe_hardware_devices) {
  EncoderCapability capability{
      .encoder_name = std::string(spec.name),
      .codec = spec.codec,
      .category = EncoderCategory::Software,
      .available = false,
      .hardware = false,
      .hardware_device_usable = true,
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
  if (capability.hardware && probe_hardware_devices) {
    const AVHWDeviceType device_type = hardware_device_type(spec.name);
    const AVPixelFormat device_pixel_format = hardware_pixel_format(spec.name);
    if (device_type != AV_HWDEVICE_TYPE_NONE && device_pixel_format != AV_PIX_FMT_NONE) {
      AVBufferRef* device = nullptr;
      capability.hardware_device_usable =
          av_hwdevice_ctx_create(&device, device_type, nullptr, nullptr, 0) >= 0 &&
          device != nullptr;
      if (capability.hardware_device_usable) {
        AVCodecContext* context = avcodec_alloc_context3(encoder);
        if (context == nullptr) {
          capability.hardware_device_usable = false;
        } else {
          context->codec_type = AVMEDIA_TYPE_VIDEO;
          context->codec_id = encoder->id;
          context->width = 64;
          context->height = 64;
          context->pix_fmt = device_pixel_format;
          context->time_base = {1, 30};
          context->framerate = {30, 1};
          context->hw_device_ctx = av_buffer_ref(device);
          AVBufferRef* frames = av_hwframe_ctx_alloc(device);
          if (frames == nullptr) {
            capability.hardware_device_usable = false;
          } else {
            auto* frame_context = reinterpret_cast<AVHWFramesContext*>(frames->data);
            frame_context->format = device_pixel_format;
            frame_context->sw_format = AV_PIX_FMT_NV12;
            frame_context->width = context->width;
            frame_context->height = context->height;
            frame_context->initial_pool_size = 4;
            capability.hardware_device_usable = av_hwframe_ctx_init(frames) >= 0;
            if (capability.hardware_device_usable) {
              context->hw_frames_ctx = av_buffer_ref(frames);
              capability.hardware_device_usable = context->hw_device_ctx != nullptr &&
                                                  context->hw_frames_ctx != nullptr &&
                                                  avcodec_open2(context, encoder, nullptr) >= 0;
            }
            av_buffer_unref(&frames);
          }
          avcodec_free_context(&context);
        }
      }
      av_buffer_unref(&device);
    } else if (capability.hardware) {
      capability.hardware_device_usable = false;
    }
  } else if (capability.hardware) {
    // The lightweight pass is deliberately limited to avcodec_find_encoder;
    // hardware readiness must not be inferred until the asynchronous device
    // probe completes. This also keeps UI availability based on software
    // encoders during startup.
    capability.hardware_device_usable = false;
  }
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
  case DeliveryCodec::Vp9:
    return "VP9";
  case DeliveryCodec::Opus:
    return "Opus";
  case DeliveryCodec::Ffv1:
    return "FFV1";
  case DeliveryCodec::ProRes:
    return "ProRes";
  }
  return "Unknown";
}

} // namespace

EncoderCapabilityMatrix probe_encoder_capabilities(const bool probe_hardware_devices) {
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
        matrix.encoders.push_back(probe_encoder(spec, probe_hardware_devices));
      } catch (...) {
        EncoderCapability unavailable{
            .encoder_name = std::string(spec.name),
            .codec = spec.codec,
            .category = EncoderCategory::Software,
            .available = false,
            .hardware = false,
            .hardware_device_usable = true,
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
    if (capability.codec != codec || !capability.available ||
        (capability.hardware && !capability.hardware_device_usable)) {
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
  return std::any_of(matrix.encoders.begin(), matrix.encoders.end(),
                     [codec](const auto& capability) {
                       return capability.codec == codec && capability.available &&
                              capability.hardware && capability.hardware_device_usable;
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
        DeliveryCodec::Vp9, DeliveryCodec::Opus, DeliveryCodec::Ffv1, DeliveryCodec::ProRes}) {
    summary += codec_name(codec) + ":\n";
    for (const EncoderCapability& capability : matrix.encoders) {
      if (capability.codec != codec) {
        continue;
      }
      summary += "  " + capability.encoder_name + ": ";
      summary += capability.available ? "available\n" : "not available\n";
      if (capability.hardware && capability.available) {
        summary +=
            capability.hardware_device_usable ? "    device: ready\n" : "    device: unavailable\n";
      }
    }
  }
  return summary;
}

} // namespace video_editor::media
