// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::export_service {

enum class VideoPreset : std::uint8_t {
  Ffv1Matroska,
  ProRes422HqMov,
};

enum class PlatformPreset : std::uint8_t {
  // Reference masters (already supported by the existing export path)
  ReferenceFfv1,
  ReferenceProRes,
  // Creator delivery presets (codec selection gated on legal approval)
  YouTube1080p,
  YouTube1440p,
  YouTube2160p,  // 4K
  Vertical1080x1920,
  Vertical720x1280,
  PodcastAudioOnly,
};

enum class CaptionExportMode : std::uint8_t {
  None,       // no caption export
  BurnIn,     // render captions into video frames
  Sidecar,    // write SRT/WebVTT alongside the media file
  BurnInAndSidecar,
};

enum class SidecarFormat : std::uint8_t { Srt, WebVtt };

struct PlatformPresetInfo final {
  PlatformPreset preset{PlatformPreset::YouTube1080p};
  std::string display_name;
  std::string intended_container;    // "mp4", "mkv", "mov", "m4a"
  std::string intended_video_codec;  // "h264", "ffv1", "prores", "" for audio-only
  std::string intended_audio_codec;  // "aac", "pcm_s16le", "mp3"
  std::uint32_t target_width{0};     // 0 = use sequence width
  std::uint32_t target_height{0};    // 0 = use sequence height
  std::uint32_t target_frame_rate_num{0};
  std::uint32_t target_frame_rate_den{1};
  std::uint64_t target_video_bitrate{0};  // bits/sec, 0 = CRF/quality-based
  std::uint32_t target_audio_bitrate{0};  // bits/sec
  bool audio_only{false};
  bool delivery_codec_approved{false};  // false until legal sign-off
  std::string notes;                    // human-readable guidance
};

[[nodiscard]] PlatformPresetInfo platform_preset_info(PlatformPreset preset);

// Returns all available platform presets in display order.
[[nodiscard]] std::vector<PlatformPresetInfo> available_platform_presets();

// Maps a PlatformPreset to the existing VideoPreset enum for the reference
// master path. Returns std::nullopt for delivery presets (they need the
// not-yet-approved H.264/AAC path).
[[nodiscard]] std::optional<VideoPreset>
reference_video_preset_for(PlatformPreset preset);

}  // namespace video_editor::export_service
