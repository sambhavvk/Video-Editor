// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/presets.h"

#include <array>
#include <cstddef>

namespace video_editor::export_service {
namespace {

const std::array kPlatformPresets{
    PlatformPresetInfo{PlatformPreset::ReferenceFfv1,
                        "Lossless master · FFV1 / Matroska",
                        "mkv",
                        "ffv1",
                        "pcm_s16le",
                        0,
                        0,
                        0,
                        1,
                        0,
                        0,
                        false,
                        true,
                        "Lossless archival master with FFV1 video and PCM audio"},
    PlatformPresetInfo{PlatformPreset::ReferenceProRes,
                       "Editing master · ProRes 422 HQ / MOV",
                       "mov",
                       "prores",
                       "pcm_s16le",
                       0,
                       0,
                       0,
                       1,
                       0,
                       0,
                       false,
                       true,
                       "Editing master with ProRes 422 HQ video and PCM audio"},
    PlatformPresetInfo{PlatformPreset::YouTube1080p,
                       "YouTube 1080p",
                       "mp4",
                       "h264",
                       "aac",
                       1920,
                       1080,
                       0,
                       1,
                       8'000'000,
                       128'000,
                       false,
                       false,
                       "Full HD YouTube delivery; H.264/AAC pending legal approval"},
    PlatformPresetInfo{PlatformPreset::YouTube1440p,
                       "YouTube 1440p",
                       "mp4",
                       "h264",
                       "aac",
                       2560,
                       1440,
                       0,
                       1,
                       16'000'000,
                       192'000,
                       false,
                       false,
                       "1440p YouTube delivery; H.264/AAC pending legal approval"},
    PlatformPresetInfo{PlatformPreset::YouTube2160p,
                       "YouTube 2160p",
                       "mp4",
                       "h264",
                       "aac",
                       3840,
                       2160,
                       0,
                       1,
                       40'000'000,
                       192'000,
                       false,
                       false,
                       "4K YouTube delivery; H.264/AAC pending legal approval"},
    PlatformPresetInfo{PlatformPreset::Vertical1080x1920,
                       "Vertical 1080x1920",
                       "mp4",
                       "h264",
                       "aac",
                       1080,
                       1920,
                       0,
                       1,
                       6'000'000,
                       128'000,
                       false,
                       false,
                       "Vertical format for Shorts/Reels/Stories; H.264/AAC pending legal approval"},
    PlatformPresetInfo{PlatformPreset::Vertical720x1280,
                       "Vertical 720x1280",
                       "mp4",
                       "h264",
                       "aac",
                       720,
                       1280,
                       0,
                       1,
                       3'000'000,
                       96'000,
                       false,
                       false,
                       "Lower-resolution vertical delivery; H.264/AAC pending legal approval"},
    PlatformPresetInfo{PlatformPreset::PodcastAudioOnly,
                       "Podcast audio only",
                       "m4a",
                       "",
                       "aac",
                       0,
                       0,
                       0,
                       1,
                       0,
                       96'000,
                       true,
                       false,
                       "Audio-only podcast delivery; H.264/AAC pending legal approval"}};

}  // namespace

PlatformPresetInfo platform_preset_info(const PlatformPreset preset) {
  const auto index = static_cast<std::size_t>(preset);
  if (index >= kPlatformPresets.size()) {
    return {};
  }
  return kPlatformPresets[index];
}

std::vector<PlatformPresetInfo> available_platform_presets() {
  return {kPlatformPresets.begin(), kPlatformPresets.end()};
}

std::optional<VideoPreset> reference_video_preset_for(const PlatformPreset preset) {
  switch (preset) {
    case PlatformPreset::ReferenceFfv1:
      return VideoPreset::Ffv1Matroska;
    case PlatformPreset::ReferenceProRes:
      return VideoPreset::ProRes422HqMov;
    case PlatformPreset::YouTube1080p:
    case PlatformPreset::YouTube1440p:
    case PlatformPreset::YouTube2160p:
    case PlatformPreset::Vertical1080x1920:
    case PlatformPreset::Vertical720x1280:
    case PlatformPreset::PodcastAudioOnly:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace video_editor::export_service
