// SPDX-License-Identifier: MPL-2.0
#include "video_editor/export_service/audio_stem_export.h"

#include <gtest/gtest.h>

namespace video_editor::export_service {
namespace {

TEST(AudioStemExportTest, RejectsInvalidRequest) {
  AudioStemExportRequest request;
  const auto result = export_production_audio_stems(request);
  EXPECT_FALSE(result);
}

} // namespace
} // namespace video_editor::export_service
