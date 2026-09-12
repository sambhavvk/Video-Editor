// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

TEST(MulticamM03Test, AllowsFourAnglesAndMissingAngleShowsBlack) {
  Project project;
  Asset asset;
  asset.duration = Time(100, 1);
  asset.has_video = true;
  project.assets.push_back(asset);
  Sequence sequence;
  sequence.name = "Four-up";
  std::vector<Clip> clips;
  for (int index = 0; index < 4; ++index) {
    Track track;
    track.kind = TrackKind::Video;
    Clip clip;
    clip.asset_id = asset.id;
    clip.kind = ClipKind::Video;
    clip.timeline_range = TimeRange(Time(index * 5, 1), Time(100, 1));
    clip.source_range = TimeRange(Time{}, Time(100, 1));
    if (index == 2) {
      clip.enabled = false;
    }
    track.clips.push_back(clip);
    clips.push_back(clip);
    sequence.tracks.push_back(track);
  }
  project.sequences.push_back(sequence);

  MulticamGroup group;
  group.sequence_id = sequence.id;
  group.name = "Four-up";
  for (int index = 0; index < 4; ++index) {
    MulticamAngle angle;
    angle.clip_id = clips[static_cast<std::size_t>(index)].id;
    angle.label = "Cam " + std::to_string(index + 1);
    group.angles.push_back(angle);
  }
  group.active_angle_id = group.angles[2].id;
  group.audio_master_angle_id = group.angles[0].id;

  TimelineEditor editor(std::move(project));
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateMulticamGroupCommand{.group = group}}));
  const auto* stored = findMulticamGroup(*editor.projectAt(editor.revision()), group.id);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->angles.size(), 4U);
  EXPECT_FALSE(multicamVideoClipVisible(*editor.projectAt(editor.revision()), sequence.id,
                                        clips[2], Time(10, 1)));
}

} // namespace
} // namespace video_editor::edit
