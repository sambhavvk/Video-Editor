// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::media {

enum class EncoderCategory : std::uint8_t {
  Software,
  HardwareNvidia, // NVENC
  HardwareIntel,  // QSV / Media Foundation
  HardwareAmd,    // AMF
  HardwareApple,  // VideoToolbox
  HardwareOther,
};

enum class DeliveryCodec : std::uint8_t {
  H264,
  Aac,
  Hevc,
  Av1,
  Vp9,
  Opus,
  // Reference/intermediate codecs already supported by export_service
  Ffv1,
  ProRes,
};

struct EncoderCapability {
  std::string encoder_name; // FFmpeg encoder name, e.g. "h264_nvenc", "libx264"
  DeliveryCodec codec{DeliveryCodec::H264};
  EncoderCategory category{EncoderCategory::Software};
  bool available{false};                            // encoder present at runtime
  bool hardware{false};                             // category != Software
  bool hardware_device_usable{true};                // runtime device can be initialized
  std::vector<std::string> supported_pixel_formats; // empty if query failed
  std::string max_profile;                          // best-effort; empty if unknown
  std::string max_level;                            // best-effort; empty if unknown
};

struct EncoderCapabilityMatrix {
  std::vector<EncoderCapability> encoders;
  bool lgpl_compatible_runtime{false}; // mirrors runtime_info().lgpl_compatible_configuration
  bool h264_delivery_approved{false};  // false until legal sign-off flips a build flag
  bool aac_delivery_approved{false};   // false until legal sign-off flips a build flag
};

// Probes all known delivery + reference encoders at runtime. Never throws.
// Returns a matrix with available=false for encoders not present.
[[nodiscard]] EncoderCapabilityMatrix
probe_encoder_capabilities(bool probe_hardware_devices = true);

// Convenience: returns the best available encoder for a codec, preferring
// hardware over software. Returns std::nullopt if none available.
[[nodiscard]] std::optional<EncoderCapability>
best_encoder_for(const EncoderCapabilityMatrix& matrix, DeliveryCodec codec);

// Returns true if at least one hardware encoder is available for the codec.
[[nodiscard]] bool has_hardware_encoder(const EncoderCapabilityMatrix& matrix, DeliveryCodec codec);

// Human-readable summary for diagnostics UI / logs.
[[nodiscard]] std::string format_capability_summary(const EncoderCapabilityMatrix& matrix);

} // namespace video_editor::media
