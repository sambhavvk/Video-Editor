// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"
#include "video_editor/edit_model/sequence_compare.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

[[nodiscard]] Clip makeClip(std::string name, Time start, Time duration) {
  Clip clip;
  clip.name = std::move(name);
  clip.timeline_range = TimeRange{start, duration};
  clip.source_range = clip.timeline_range;
  return clip;
}

TEST(V01SequenceVersionsTest, CompareDetectsAddedClip) {
  Sequence before;
  before.name = "Main";
  Track track;
  track.kind = TrackKind::Video;
  before.tracks.push_back(track);

  Sequence after = before;
  after.tracks.front().clips.push_back(makeClip("Interview", Time(0, 1), Time(24, 1)));

  const auto changes = compare_sequences(before, after);
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes.front().kind, SequenceChangeKind::ClipAdded);
}

TEST(V01SequenceVersionsTest, CompareDetectsMovedRangeRemovedAndUnsupported) {
  Sequence before;
  before.name = "Main";
  Track track;
  track.kind = TrackKind::Video;
  Clip kept = makeClip("Interview", Time(0, 1), Time(24, 1));
  Clip removed = makeClip("B-roll", Time(24, 1), Time(12, 1));
  track.clips.push_back(kept);
  track.clips.push_back(removed);
  before.tracks.push_back(track);

  Sequence after = before;
  after.tracks.front().clips.pop_back();
  after.tracks.front().clips.front().timeline_range = TimeRange{Time(10, 1), Time(18, 1)};
  after.tracks.front().clips.front().source_range = TimeRange{Time(2, 1), Time(18, 1)};
  after.tracks.front().clips.front().playback_rate = Rate(2, 1);
  Transition transition;
  transition.outgoing_clip_id = kept.id;
  transition.incoming_clip_id = removed.id;
  transition.range = TimeRange{Time(20, 1), Time(2, 1)};
  after.transitions.push_back(transition);

  const auto changes = compare_sequences(before, after);
  bool moved = false;
  bool range_changed = false;
  bool source_changed = false;
  bool removed_named = false;
  bool unsupported_retime = false;
  bool unsupported_transition = false;
  for (const auto& change : changes) {
    moved = moved || (change.kind == SequenceChangeKind::ClipMoved &&
                      change.detail == "timeline position changed");
    range_changed = range_changed || (change.kind == SequenceChangeKind::ClipMoved &&
                                      change.detail == "timeline range changed");
    source_changed = source_changed || (change.kind == SequenceChangeKind::ClipMoved &&
                                        change.detail == "source range changed");
    removed_named = removed_named || (change.kind == SequenceChangeKind::ClipRemoved &&
                                      change.clip_name == "B-roll");
    unsupported_retime = unsupported_retime || (change.kind == SequenceChangeKind::Unsupported &&
                                                change.detail.find("retime") != std::string::npos);
    unsupported_transition =
        unsupported_transition || (change.kind == SequenceChangeKind::Unsupported &&
                                   change.detail.find("transition") != std::string::npos);
  }
  EXPECT_TRUE(moved);
  EXPECT_TRUE(range_changed);
  EXPECT_TRUE(source_changed);
  EXPECT_TRUE(removed_named);
  EXPECT_TRUE(unsupported_retime);
  EXPECT_TRUE(unsupported_transition);
}

TEST(V01SequenceVersionsTest, NamedVersionSnapshotIsStored) {
  Project project;
  Sequence live;
  live.name = "Cut";
  project.sequences.push_back(live);
  TimelineEditor editor(std::move(project));

  Sequence snapshot = live;
  snapshot.id = EntityId::generate();
  NamedSequenceVersion version;
  version.name = "Director cut";
  version.sequence_id = snapshot.id;
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateSequenceVersionCommand{
                                              .version = version,
                                              .sequence_snapshot = snapshot}}));
  EXPECT_EQ(editor.projectAt(editor.revision())->sequence_versions.size(), 1U);
}

TEST(V01SequenceVersionsTest, NamedVersionSnapshotWithClipsIsStored) {
  Project project;
  Asset asset;
  asset.name = "interview.mov";
  asset.source_uri = "memory://interview";
  asset.duration = Time(48, 1);
  asset.has_video = true;
  asset.width = 1920;
  asset.height = 1080;
  project.assets.push_back(asset);

  Sequence live;
  live.name = "Cut";
  Track track;
  track.kind = TrackKind::Video;
  Clip clip = makeClip("Interview", Time(0, 1), Time(24, 1));
  clip.asset_id = asset.id;
  track.clips.push_back(clip);
  live.tracks.push_back(track);
  project.sequences.push_back(live);
  TimelineEditor editor(std::move(project));

  Sequence snapshot = live;
  snapshot.id = EntityId::generate();
  NamedSequenceVersion version;
  version.name = "Director cut";
  version.sequence_id = snapshot.id;
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateSequenceVersionCommand{
                                              .version = version,
                                              .sequence_snapshot = snapshot}}));
  const auto stored = editor.projectAt(editor.revision());
  ASSERT_EQ(stored->sequence_versions.size(), 1U);
  ASSERT_EQ(stored->sequences.size(), 2U);
  const Sequence* snapshot_sequence = findSequence(*stored, version.sequence_id);
  ASSERT_NE(snapshot_sequence, nullptr);
  ASSERT_FALSE(snapshot_sequence->tracks.empty());
  ASSERT_FALSE(snapshot_sequence->tracks.front().clips.empty());
  EXPECT_NE(snapshot_sequence->tracks.front().clips.front().id, clip.id);
  EXPECT_EQ(snapshot_sequence->tracks.front().clips.front().name, "Interview");
}

TEST(V01SequenceVersionsTest, CreateSequenceVersionRejectsMismatchedIds) {
  Project project;
  Sequence live;
  live.name = "Cut";
  project.sequences.push_back(live);
  TimelineEditor editor(std::move(project));

  Sequence snapshot = live;
  snapshot.id = EntityId::generate();
  NamedSequenceVersion version;
  version.name = "Director cut";
  version.sequence_id = EntityId::generate();
  EXPECT_FALSE(applyOk(editor, EditCommand{.operation = CreateSequenceVersionCommand{
                                               .version = version,
                                               .sequence_snapshot = snapshot}}));
}

} // namespace
} // namespace video_editor::edit
