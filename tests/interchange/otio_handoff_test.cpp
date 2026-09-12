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
  clip.playback_rate = edit::Rate(2, 1);
  track.clips.push_back(clip);

  edit::Sequence nested;
  nested.name = "Inner";
  nested.tracks.push_back(edit::Track{});
  nested.tracks.back().kind = edit::TrackKind::Video;
  project.sequences.push_back(nested);
  edit::Clip nested_clip;
  nested_clip.kind = edit::ClipKind::NestedSequence;
  nested_clip.name = "Nest";
  nested_clip.nested_sequence_id = nested.id;
  nested_clip.timeline_range = edit::TimeRange{edit::Time(24, 1), edit::Time(12, 1)};
  nested_clip.source_range = nested_clip.timeline_range;
  track.clips.push_back(nested_clip);
  sequence.tracks.push_back(track);
  project.sequences.insert(project.sequences.begin(), sequence);

  const auto temp = std::filesystem::temp_directory_path() / "ve_otio_handoff_test";
  std::filesystem::remove_all(temp);
  const auto result = build_otio_handoff_package(project, sequence.id, temp, "OpenTimelineIO");
  ASSERT_TRUE(result);
  EXPECT_TRUE(std::filesystem::exists(result.value().timeline_path));
  EXPECT_TRUE(std::filesystem::exists(result.value().report_path));
  EXPECT_TRUE(std::filesystem::exists(result.value().manifest_path));
  EXPECT_FALSE(result.value().report.supported.empty());
  EXPECT_TRUE(result.value().report.flattened.empty());
  bool reports_nested = false;
  for (const auto& entry : result.value().report.supported) {
    reports_nested = reports_nested || entry.find("Nested sequence") != std::string::npos;
  }
  EXPECT_TRUE(reports_nested);
  bool reports_retime = false;
  for (const auto& entry : result.value().report.omitted) {
    reports_retime = reports_retime || entry.find("Retime") != std::string::npos;
  }
  EXPECT_TRUE(reports_retime);
  std::filesystem::remove_all(temp);
}

} // namespace
} // namespace video_editor::interchange
