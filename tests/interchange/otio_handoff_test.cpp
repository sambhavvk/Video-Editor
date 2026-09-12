// SPDX-License-Identifier: MPL-2.0
#include "video_editor/interchange/otio_handoff.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace video_editor::interchange {
namespace {

TEST(OtioHandoffTest, BuildsPackageWithReportSections) {
  edit::Project project;
  edit::Asset asset;
  asset.name = "clip.mov";
  asset.source_uri = "memory://clip";
  asset.duration = edit::Time(24, 1);
  asset.has_video = true;
  asset.width = 1920;
  asset.height = 1080;
  project.assets.push_back(asset);
  edit::Sequence sequence;
  sequence.name = "Main";
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  edit::Clip clip;
  clip.asset_id = asset.id;
  clip.kind = edit::ClipKind::Video;
  clip.timeline_range = edit::TimeRange{edit::Time(0, 1), edit::Time(24, 1)};
  clip.source_range = clip.timeline_range;
  track.clips.push_back(clip);
  sequence.tracks.push_back(track);
  project.sequences.push_back(sequence);

  const auto temp = std::filesystem::temp_directory_path() / "ve_otio_handoff_test";
  std::filesystem::remove_all(temp);
  const auto result = build_otio_handoff_package(project, sequence.id, temp, "OpenTimelineIO");
  ASSERT_TRUE(result);
  EXPECT_TRUE(std::filesystem::exists(result.value().timeline_path));
  EXPECT_TRUE(std::filesystem::exists(result.value().report_path));
  EXPECT_FALSE(result.value().report.supported.empty());
  std::filesystem::remove_all(temp);
}

} // namespace
} // namespace video_editor::interchange
