// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/presets.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <gtest/gtest.h>

namespace video_editor::export_service {
namespace {

TEST(export_presets, AvailablePresetsAreInDisplayOrder) {
  const auto presets = available_platform_presets();

  ASSERT_EQ(presets.size(), 8U);
  EXPECT_EQ(presets[0].preset, PlatformPreset::ReferenceFfv1);
  EXPECT_EQ(presets[1].preset, PlatformPreset::ReferenceProRes);
  EXPECT_EQ(presets[2].preset, PlatformPreset::YouTube1080p);
  EXPECT_EQ(presets[3].preset, PlatformPreset::YouTube1440p);
  EXPECT_EQ(presets[4].preset, PlatformPreset::YouTube2160p);
  EXPECT_EQ(presets[5].preset, PlatformPreset::Vertical1080x1920);
  EXPECT_EQ(presets[6].preset, PlatformPreset::Vertical720x1280);
  EXPECT_EQ(presets[7].preset, PlatformPreset::PodcastAudioOnly);
}

TEST(export_presets, YouTube1080pHasExpectedDeliverySettings) {
  const auto info = platform_preset_info(PlatformPreset::YouTube1080p);

  EXPECT_EQ(info.target_width, 1920U);
  EXPECT_EQ(info.target_height, 1080U);
  EXPECT_EQ(info.target_video_bitrate, 8'000'000U);
  EXPECT_EQ(info.intended_container, "webm");
  EXPECT_EQ(info.intended_video_codec, "vp9");
  EXPECT_EQ(info.intended_audio_codec, "opus");
  EXPECT_TRUE(info.delivery_codec_approved);
}

TEST(export_presets, ReferenceFfv1IsApprovedAndMapsToReferencePreset) {
  const auto info = platform_preset_info(PlatformPreset::ReferenceFfv1);

  EXPECT_TRUE(info.delivery_codec_approved);
  ASSERT_TRUE(reference_video_preset_for(PlatformPreset::ReferenceFfv1).has_value());
  EXPECT_EQ(*reference_video_preset_for(PlatformPreset::ReferenceFfv1), VideoPreset::Ffv1Matroska);
}

TEST(export_presets, DeliveryPresetDoesNotMapToReferencePreset) {
  ASSERT_TRUE(reference_video_preset_for(PlatformPreset::YouTube1080p).has_value());
  EXPECT_EQ(*reference_video_preset_for(PlatformPreset::YouTube1080p), VideoPreset::Vp9OpusWebm);
}

TEST(export_presets, PodcastPresetIsAudioOnly) {
  const auto info = platform_preset_info(PlatformPreset::PodcastAudioOnly);

  EXPECT_TRUE(info.audio_only);
  EXPECT_TRUE(info.intended_video_codec.empty());
}

TEST(export_presets, FossDeliveryAvailabilityRequiresRuntimeVideoAndAudioEncoders) {
  EXPECT_TRUE(platform_preset_available(PlatformPreset::YouTube1080p));
  EXPECT_TRUE(platform_preset_available(PlatformPreset::PodcastAudioOnly));
}

TEST(export_presets, CreatorAv1AvailabilityRequiresAv1AndOpusEncoders) {
  const bool expected =
      (avcodec_find_encoder_by_name("libsvtav1") != nullptr ||
       avcodec_find_encoder_by_name("libaom-av1") != nullptr) &&
      (avcodec_find_encoder_by_name("libopus") != nullptr ||
       avcodec_find_encoder(AV_CODEC_ID_OPUS) != nullptr);
  EXPECT_EQ(creator_av1_available(), expected);
}

TEST(export_presets, CreatorVideoPresetForYouTubeUsesSelectedCodec) {
  EXPECT_EQ(creator_video_preset_for(PlatformPreset::YouTube1080p, CreatorVideoCodec::Vp9),
            VideoPreset::Vp9OpusWebm);
  EXPECT_EQ(creator_video_preset_for(PlatformPreset::YouTube1080p, CreatorVideoCodec::Av1),
            VideoPreset::Av1OpusWebm);
  EXPECT_EQ(creator_video_preset_for(PlatformPreset::PodcastAudioOnly, CreatorVideoCodec::Av1),
            VideoPreset::Vp9OpusWebm);
  EXPECT_EQ(creator_video_preset_for(PlatformPreset::ReferenceFfv1, CreatorVideoCodec::Av1),
            VideoPreset::Ffv1Matroska);
}

TEST(export_presets, CaptionModeHelpersClassifyEmbeddedModes) {
  EXPECT_TRUE(caption_mode_embeds(CaptionExportMode::Embedded));
  EXPECT_TRUE(caption_mode_embeds(CaptionExportMode::BurnInAndEmbedded));
  EXPECT_FALSE(caption_mode_embeds(CaptionExportMode::Sidecar));
  EXPECT_TRUE(caption_mode_burns_in(CaptionExportMode::BurnInAndEmbedded));
  EXPECT_TRUE(caption_mode_writes_sidecar(CaptionExportMode::BurnInAndSidecar));
}

TEST(export_presets, AllDisplayNamesAreNonEmpty) {
  for (const auto& info : available_platform_presets()) {
    EXPECT_FALSE(info.display_name.empty());
  }
}

} // namespace
} // namespace video_editor::export_service
