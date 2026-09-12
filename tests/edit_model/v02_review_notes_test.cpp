// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"
#include "video_editor/project_codec/project_codec.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

[[nodiscard]] NamedSequenceVersion makeVersion(Sequence& snapshot, std::string name) {
  snapshot.id = EntityId::generate();
  NamedSequenceVersion version;
  version.name = std::move(name);
  version.sequence_id = snapshot.id;
  return version;
}

TEST(V02ReviewNotesTest, NoteStoresContextAndResolve) {
  Project project;
  Sequence live;
  live.name = "Cut";
  project.sequences.push_back(live);
  TimelineEditor editor(std::move(project));

  Sequence snapshot = live;
  const NamedSequenceVersion version = makeVersion(snapshot, "Director cut");
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateSequenceVersionCommand{
                                              .version = version,
                                              .sequence_snapshot = snapshot}}));

  ReviewNote note;
  note.sequence_version_id = version.id;
  note.author = "sea";
  note.body = "Check eyeline on take 3";
  note.source_range = TimeRange{Time(10, 1), Time(2, 1)};
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = AddReviewNoteCommand{.note = note}}));
  ASSERT_EQ(editor.projectAt(editor.revision())->review_notes.size(), 1U);
  EXPECT_FALSE(editor.projectAt(editor.revision())->review_notes.front().needs_reconciliation);

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = ResolveReviewNoteCommand{
                                              .note_id = note.id,
                                              .resolved = true}}));
  EXPECT_TRUE(editor.projectAt(editor.revision())->review_notes.front().resolved);
  EXPECT_FALSE(editor.projectAt(editor.revision())->review_notes.front().needs_reconciliation);
}

TEST(V02ReviewNotesTest, NoteFlagsReconciliationAndRoundTrips) {
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
  project.sequences.push_back(live);
  TimelineEditor editor(std::move(project));

  Sequence snapshot = live;
  const NamedSequenceVersion version = makeVersion(snapshot, "Director cut");
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateSequenceVersionCommand{
                                              .version = version,
                                              .sequence_snapshot = snapshot}}));

  ReviewNote note;
  note.sequence_version_id = version.id;
  note.asset_id = EntityId::generate();
  note.author = "sea";
  note.body = "Missing take";
  note.source_range = TimeRange{Time(10, 1), Time(2, 1)};
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = AddReviewNoteCommand{.note = note}}));
  ASSERT_EQ(editor.projectAt(editor.revision())->review_notes.size(), 1U);
  EXPECT_TRUE(editor.projectAt(editor.revision())->review_notes.front().needs_reconciliation);

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = ResolveReviewNoteCommand{
                                              .note_id = note.id,
                                              .resolved = true}}));
  const auto stored = *editor.projectAt(editor.revision());
  EXPECT_TRUE(stored.review_notes.front().resolved);
  EXPECT_FALSE(stored.review_notes.front().needs_reconciliation);

  const auto bytes = project_codec::serialize_project(stored);
  auto decoded = project_codec::deserialize_project(bytes);
  ASSERT_TRUE(decoded);
  ASSERT_EQ(decoded.value().review_notes.size(), 1U);
  EXPECT_EQ(decoded.value().review_notes.front().author, "sea");
  EXPECT_EQ(decoded.value().review_notes.front().body, "Missing take");
  EXPECT_TRUE(decoded.value().review_notes.front().resolved);
  EXPECT_EQ(decoded.value().review_notes.front().source_range->duration, Time(2, 1));
  EXPECT_EQ(decoded.value().sequence_versions.front().name, "Director cut");
}

} // namespace
} // namespace video_editor::edit
