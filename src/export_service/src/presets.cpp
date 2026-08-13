// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/presets.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <array>
#include <cstddef>

namespace video_editor::export_service {
namespace {

const std::array kPlatformPresets{
    PlatformPresetInfo{PlatformPreset::ReferenceFfv1, "Lossless master · FFV1 / Matroska", "mkv",
                       "ffv1", "pcm_s16le", 0, 0, 0, 1, 0, 0, false, true,
                       "Lossless archival master with FFV1 video and PCM audio"},
    PlatformPresetInfo{PlatformPreset::ReferenceProRes, "Editing master · ProRes 422 HQ / MOV",
                       "mov", "prores", "pcm_s16le", 0, 0, 0, 1, 0, 0, false, true,
                       "Editing master with ProRes 422 HQ video and PCM audio"},
    PlatformPresetInfo{PlatformPreset::YouTube1080p, "YouTube 1080p", "webm", "vp9", "opus", 1920,
                       1080, 0, 1, 8'000'000, 128'000, false, true,
                       "Full HD creator delivery; FOSS VP9 video and Opus audio in WebM"},
    PlatformPresetInfo{PlatformPreset::YouTube1440p, "YouTube 1440p", "webm", "vp9", "opus", 2560,
                       1440, 0, 1, 16'000'000, 192'000, false, true,
                       "1440p creator delivery; FOSS VP9 video and Opus audio in WebM"},
    PlatformPresetInfo{PlatformPreset::YouTube2160p, "YouTube 2160p", "webm", "vp9", "opus", 3840,
                       2160, 0, 1, 40'000'000, 192'000, false, true,
                       "4K creator delivery; FOSS VP9 video and Opus audio in WebM"},
    PlatformPresetInfo{PlatformPreset::Vertical1080x1920, "Vertical 1080x1920", "webm", "vp9",
                       "opus", 1080, 1920, 0, 1, 6'000'000, 128'000, false, true,
                       "Vertical creator delivery; FOSS VP9 video and Opus audio in WebM"},
    PlatformPresetInfo{PlatformPreset::Vertical720x1280, "Vertical 720x1280", "webm", "vp9", "opus",
                       720, 1280, 0, 1, 3'000'000, 96'000, false, true,
                       "Lower-resolution vertical delivery; FOSS VP9 video and Opus audio in WebM"},
    PlatformPresetInfo{PlatformPreset::PodcastAudioOnly, "Podcast audio only", "webm", "", "opus",
                       0, 0, 0, 1, 0, 96'000, true, true,
                       "Audio-only podcast delivery using FOSS Opus in WebM"}};

} // namespace

PlatformPresetInfo platform_preset_info(const PlatformPreset preset) {
  const auto index = static_cast<std::size_t>(preset);
  if (index >= kPlatformPresets.size()) {
    return {};
  }
  return kPlatformPresets[index];
}

bool platform_preset_available(const PlatformPreset preset) noexcept {
  if (preset == PlatformPreset::ReferenceFfv1) {
    return avcodec_find_encoder(AV_CODEC_ID_FFV1) != nullptr;
  }
  if (preset == PlatformPreset::ReferenceProRes) {
    return avcodec_find_encoder_by_name("prores_ks") != nullptr ||
           avcodec_find_encoder_by_name("prores_aw") != nullptr;
  }
  const bool video_available = avcodec_find_encoder_by_name("libvpx-vp9") != nullptr;
  const bool audio_available = avcodec_find_encoder_by_name("libopus") != nullptr ||
                               avcodec_find_encoder(AV_CODEC_ID_OPUS) != nullptr;
  return video_available && audio_available;
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
    return VideoPreset::Vp9OpusWebm;
  }
  return std::nullopt;
}

} // namespace video_editor::export_service
