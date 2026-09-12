// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] Project makeTwoClipProject() {
  Project project;
  Asset asset_a;
  asset_a.name = "camera_a.mov";
  asset_a.source_uri = "memory://camera_a";
  asset_a.duration = Time(100, 1);
  asset_a.has_video = true;
  asset_a.width = 1'920;
  asset_a.height = 1'080;
  Asset asset_b = asset_a;
  asset_b.id = EntityId::generate();
  asset_b.name = "camera_b.mov";
  asset_b.source_uri = "memory://camera_b";
  project.assets = {asset_a, asset_b};

  Sequence sequence;
  sequence.name = "Interview";
  Track video_a;
  video_a.kind = TrackKind::Video;
  Track video_b;
  video_b.kind = TrackKind::Video;
  Clip clip_a;
  clip_a.asset_id = asset_a.id;
  clip_a.kind = ClipKind::Video;
  clip_a.name = "Camera A";
  clip_a.timeline_range = TimeRange(Time{}, Time(100, 1));
  clip_a.source_range = TimeRange(Time{}, Time(100, 1));
  Clip clip_b;
  clip_b.asset_id = asset_b.id;
  clip_b.kind = ClipKind::Video;
  clip_b.name = "Camera B";
  clip_b.timeline_range = TimeRange(Time(5, 1), Time(100, 1));
  clip_b.source_range = TimeRange(Time{}, Time(100, 1));
  video_a.clips.push_back(clip_a);
  video_b.clips.push_back(clip_b);
  sequence.tracks = {video_a, video_b};
  project.sequences.push_back(sequence);
  return project;
}

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

[[nodiscard]] MulticamGroup makeGroup(const Project& project) {
  const Sequence& sequence = project.sequences.front();
  const Clip& clip_a = sequence.tracks[0].clips.front();
  const Clip& clip_b = sequence.tracks[1].clips.front();
  MulticamAngle angle_a;
  angle_a.clip_id = clip_a.id;
  angle_a.label = "A";
  angle_a.sync_offset = clip_a.timeline_range.start - Time{};
  MulticamAngle angle_b;
  angle_b.clip_id = clip_b.id;
  angle_b.label = "B";
  angle_b.sync_offset = clip_b.timeline_range.start - Time{};
  MulticamGroup group;
  group.sequence_id = sequence.id;
  group.name = "Interview multicam";
  group.angles = {angle_a, angle_b};
  group.active_angle_id = angle_a.id;
  group.audio_master_angle_id = angle_a.id;
  group.sync_reference = Time{};
  return group;
}

TEST(MulticamM01Test, CreateRemoveAndPersistTwoAngleGroup) {
  auto project = makeTwoClipProject();
  TimelineEditor editor(std::move(project));
  const auto group = makeGroup(*editor.projectAt(editor.revision()));
  const auto group_id = group.id;
  const auto angle_a_id = group.angles[0].id;

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateMulticamGroupCommand{.group = group}}));
  const auto* stored = findMulticamGroup(*editor.projectAt(editor.revision()), group_id);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->angles.size(), 2U);
  EXPECT_EQ(stored->active_angle_id, angle_a_id);
  EXPECT_EQ(stored->audio_master_angle_id, angle_a_id);

  ASSERT_TRUE(applyOk(editor,
                      EditCommand{.operation = SetMulticamAudioMasterCommand{
                                      .group_id = group_id,
                                      .angle_id = stored->angles[1].id}}));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = SetMulticamActiveAngleCommand{
                                              .group_id = group_id,
                                              .angle_id = stored->angles[1].id}}));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = SetMulticamSyncCommand{
                                              .group_id = group_id,
                                              .sync_reference = Time(10, 1),
                                              .angle_offsets = {{stored->angles[0].id, Time(0, 1)},
                                                                {stored->angles[1].id, Time(2, 1)}}}}));

  const auto* updated = findMulticamGroup(*editor.projectAt(editor.revision()), group_id);
  ASSERT_NE(updated, nullptr);
  EXPECT_EQ(updated->sync_reference, Time(10, 1));
  EXPECT_EQ(updated->audio_master_angle_id, stored->angles[1].id);
  EXPECT_EQ(updated->active_angle_id, stored->angles[1].id);

  const auto revision_before = editor.revision().value;
  ASSERT_TRUE(applyOk(editor,
                      EditCommand{.operation = RemoveMulticamGroupCommand{.group_id = group_id}}));
  EXPECT_EQ(editor.revision().value, revision_before + 1U);
  EXPECT_EQ(findMulticamGroup(*editor.projectAt(editor.revision()), group_id), nullptr);

  const auto undo = editor.undo(editor.revision());
  ASSERT_TRUE(static_cast<bool>(undo));
  EXPECT_NE(findMulticamGroup(*editor.projectAt(editor.revision()), group_id), nullptr);
}

TEST(MulticamM01Test, RejectsDuplicateClipMembership) {
  auto project = makeTwoClipProject();
  TimelineEditor editor(std::move(project));
  const auto group = makeGroup(*editor.projectAt(editor.revision()));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateMulticamGroupCommand{.group = group}}));

  MulticamGroup duplicate = group;
  duplicate.id = EntityId::generate();
  duplicate.angles[0].id = EntityId::generate();
  duplicate.angles[1].id = EntityId::generate();
  EXPECT_FALSE(applyOk(editor, EditCommand{.operation = CreateMulticamGroupCommand{.group = duplicate}}));
}

} // namespace
} // namespace video_editor::edit
