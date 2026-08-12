// SPDX-License-Identifier: MPL-2.0
#include "video_editor/media_codec/encoder_capabilities.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace video_editor::media {
namespace {

TEST(EncoderCapabilities, ProbeReturnsNonEmptyMatrix) {
  const EncoderCapabilityMatrix matrix = probe_encoder_capabilities();
  EXPECT_FALSE(matrix.encoders.empty());
}

TEST(EncoderCapabilities, ReferenceEncodersAreAvailable) {
  const EncoderCapabilityMatrix matrix = probe_encoder_capabilities();

  EXPECT_NE(std::find_if(matrix.encoders.begin(), matrix.encoders.end(),
                         [](const auto& capability) {
                           return capability.codec == DeliveryCodec::Ffv1 && capability.available;
                         }),
            matrix.encoders.end());
  EXPECT_NE(std::find_if(matrix.encoders.begin(), matrix.encoders.end(),
                         [](const auto& capability) {
                           return capability.codec == DeliveryCodec::ProRes && capability.available;
                         }),
            matrix.encoders.end());
}

TEST(EncoderCapabilities, BestEncoderForFfv1IsAvailable) {
  const EncoderCapabilityMatrix matrix = probe_encoder_capabilities();
  const std::optional<EncoderCapability> best = best_encoder_for(matrix, DeliveryCodec::Ffv1);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->codec, DeliveryCodec::Ffv1);
  EXPECT_TRUE(best->available);
}

TEST(EncoderCapabilities, Ffv1HasNoHardwareEncoder) {
  const EncoderCapabilityMatrix matrix = probe_encoder_capabilities();
  EXPECT_FALSE(has_hardware_encoder(matrix, DeliveryCodec::Ffv1));
}

TEST(EncoderCapabilities, DeliveryLegalGatesDefaultToFalse) {
  const EncoderCapabilityMatrix matrix = probe_encoder_capabilities();
  EXPECT_FALSE(matrix.h264_delivery_approved);
  EXPECT_FALSE(matrix.aac_delivery_approved);
}

TEST(EncoderCapabilities, SummaryContainsFfv1) {
  const EncoderCapabilityMatrix matrix = probe_encoder_capabilities();
  const std::string summary = format_capability_summary(matrix);

  EXPECT_FALSE(summary.empty());
  EXPECT_NE(summary.find("FFV1"), std::string::npos);
}

TEST(EncoderCapabilities, MissingSoftwareEncoderIsRepresentedSafely) {
  const EncoderCapabilityMatrix matrix = probe_encoder_capabilities();
  const auto libx264 =
      std::find_if(matrix.encoders.begin(), matrix.encoders.end(),
                   [](const auto& capability) { return capability.encoder_name == "libx264"; });

  ASSERT_NE(libx264, matrix.encoders.end());
  EXPECT_EQ(libx264->category, EncoderCategory::Software);
}

} // namespace
} // namespace video_editor::media
