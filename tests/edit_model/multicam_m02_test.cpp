// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

TEST(MulticamM02Test, RecordsSwitchAtPlayheadAndSelectsActiveAngle) {
  Project project;
  Asset asset;
  asset.duration = Time(100, 1);
  asset.has_video = true;
  project.assets.push_back(asset);
  Sequence sequence;
  sequence.name = "Interview";
  Track video_a;
  video_a.kind = TrackKind::Video;
  Track video_b;
  video_b.kind = TrackKind::Video;
  Clip clip_a;
  clip_a.asset_id = asset.id;
  clip_a.kind = ClipKind::Video;
  clip_a.timeline_range = TimeRange(Time{}, Time(100, 1));
  clip_a.source_range = TimeRange(Time{}, Time(100, 1));
  Clip clip_b = clip_a;
  clip_b.id = EntityId::generate();
  clip_b.asset_id = asset.id;
  video_a.clips.push_back(clip_a);
  video_b.clips.push_back(clip_b);
  sequence.tracks = {video_a, video_b};
  project.sequences.push_back(sequence);

  MulticamGroup group;
  group.sequence_id = sequence.id;
  group.name = "Two-up";
  MulticamAngle angle_a;
  angle_a.clip_id = clip_a.id;
  angle_a.label = "A";
  MulticamAngle angle_b;
  angle_b.clip_id = clip_b.id;
  angle_b.label = "B";
  group.angles = {angle_a, angle_b};
  group.active_angle_id = angle_a.id;
  group.audio_master_angle_id = angle_a.id;

  TimelineEditor editor(std::move(project));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateMulticamGroupCommand{.group = group}}));
  const auto* stored = findMulticamGroup(*editor.projectAt(editor.revision()), group.id);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(activeMulticamAngleId(*stored, Time(10, 1)), angle_a.id);

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = RecordMulticamSwitchCommand{
                                              .group_id = group.id,
                                              .time = Time(10, 1),
                                              .angle_id = angle_b.id}}));
  stored = findMulticamGroup(*editor.projectAt(editor.revision()), group.id);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->switches.size(), 1U);
  EXPECT_EQ(activeMulticamAngleId(*stored, Time(10, 1)), angle_b.id);
  EXPECT_EQ(activeMulticamAngleId(*stored, Time(5, 1)), angle_a.id);
  EXPECT_TRUE(multicamVideoClipVisible(*editor.projectAt(editor.revision()), sequence.id, clip_a,
                                       Time(5, 1)));
  EXPECT_FALSE(multicamVideoClipVisible(*editor.projectAt(editor.revision()), sequence.id, clip_a,
                                        Time(10, 1)));
}

TEST(MulticamM02Test, RejectsDuplicateSwitchTimes) {
  Project project;
  Asset asset;
  asset.duration = Time(100, 1);
  asset.has_video = true;
  project.assets.push_back(asset);
  Sequence sequence;
  sequence.name = "Interview";
  Track video_a;
  video_a.kind = TrackKind::Video;
  Track video_b;
  video_b.kind = TrackKind::Video;
  Clip clip_a;
  clip_a.asset_id = asset.id;
  clip_a.kind = ClipKind::Video;
  clip_a.timeline_range = TimeRange(Time{}, Time(100, 1));
  clip_a.source_range = TimeRange(Time{}, Time(100, 1));
  Clip clip_b = clip_a;
  clip_b.id = EntityId::generate();
  video_a.clips.push_back(clip_a);
  video_b.clips.push_back(clip_b);
  sequence.tracks = {video_a, video_b};
  project.sequences.push_back(sequence);

  MulticamGroup group;
  group.sequence_id = sequence.id;
  group.name = "Two-up";
  MulticamAngle angle_a;
  angle_a.clip_id = clip_a.id;
  MulticamAngle angle_b;
  angle_b.clip_id = clip_b.id;
  group.angles = {angle_a, angle_b};
  group.active_angle_id = angle_a.id;
  group.audio_master_angle_id = angle_a.id;

  TimelineEditor editor(std::move(project));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateMulticamGroupCommand{.group = group}}));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = RecordMulticamSwitchCommand{
                                              .group_id = group.id,
                                              .time = Time(10, 1),
                                              .angle_id = angle_b.id}}));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = RecordMulticamSwitchCommand{
                                              .group_id = group.id,
                                              .time = Time(20, 1),
                                              .angle_id = angle_a.id}}));
  const auto* stored = findMulticamGroup(*editor.projectAt(editor.revision()), group.id);
  ASSERT_NE(stored, nullptr);
  ASSERT_EQ(stored->switches.size(), 2U);
  EXPECT_FALSE(applyOk(editor, EditCommand{.operation = UpdateMulticamSwitchCommand{
                                               .group_id = group.id,
                                               .switch_id = stored->switches.back().id,
                                               .time = Time(10, 1),
                                               .angle_id = angle_a.id}}));
}

} // namespace
} // namespace video_editor::edit
