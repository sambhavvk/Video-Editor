// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"
#include "video_editor/edit_model/sequence_compare.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

TEST(V01SequenceVersionsTest, CompareDetectsAddedClip) {
  Sequence before;
  before.name = "Main";
  Track track;
  track.kind = TrackKind::Video;
  before.tracks.push_back(track);

  Sequence after = before;
  Clip clip;
  clip.name = "Interview";
  clip.timeline_range = TimeRange{Time(0, 1), Time(24, 1)};
  clip.source_range = clip.timeline_range;
  after.tracks.front().clips.push_back(clip);

  const auto changes = compare_sequences(before, after);
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes.front().kind, SequenceChangeKind::ClipAdded);
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

} // namespace
} // namespace video_editor::edit
