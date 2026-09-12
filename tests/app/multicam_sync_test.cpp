// SPDX-License-Identifier: MPL-2.0
#include "multicam_sync.hpp"

#include <gtest/gtest.h>

namespace video_editor::app {
namespace {

TEST(MulticamTimecodeSyncTest, ProposesOffsetsFromEarliestSourceClock) {
  edit::Project project;
  edit::Asset asset_a;
  asset_a.metadata["timecode_start_us"] = "1000000";
  asset_a.duration = edit::Time(100, 1);
  asset_a.has_video = true;
  edit::Asset asset_b = asset_a;
  asset_b.id = edit::EntityId::generate();
  asset_b.metadata["timecode_start_us"] = "2500000";
  project.assets = {asset_a, asset_b};

  edit::Sequence sequence;
  edit::Track track_a;
  track_a.kind = edit::TrackKind::Video;
  edit::Track track_b;
  track_b.kind = edit::TrackKind::Video;
  edit::Clip clip_a;
  clip_a.asset_id = asset_a.id;
  clip_a.kind = edit::ClipKind::Video;
  clip_a.timeline_range = edit::TimeRange(edit::Time{}, edit::Time(100, 1));
  clip_a.source_range = clip_a.timeline_range;
  edit::Clip clip_b = clip_a;
  clip_b.id = edit::EntityId::generate();
  clip_b.asset_id = asset_b.id;
  clip_b.timeline_range = edit::TimeRange(edit::Time(10, 1), edit::Time(100, 1));
  track_a.clips.push_back(clip_a);
  track_b.clips.push_back(clip_b);
  sequence.tracks = {track_a, track_b};
  project.sequences.push_back(sequence);

  edit::MulticamGroup group;
  group.sequence_id = sequence.id;
  edit::MulticamAngle angle_a;
  angle_a.clip_id = clip_a.id;
  edit::MulticamAngle angle_b;
  angle_b.clip_id = clip_b.id;
  group.angles = {angle_a, angle_b};
  group.active_angle_id = angle_a.id;
  group.audio_master_angle_id = angle_a.id;

  const auto proposal =
      proposeMulticamTimecodeSync(project, sequence, group, edit::Time{});
  ASSERT_FALSE(proposal.requires_manual_choice);
  ASSERT_EQ(proposal.angles.size(), 2U);
  EXPECT_TRUE(proposal.angles[0].has_timecode);
  EXPECT_TRUE(proposal.angles[1].has_timecode);
  EXPECT_EQ(proposal.angles[0].sync_offset, edit::Time(0, 1'000'000));
}

} // namespace
} // namespace video_editor::app
