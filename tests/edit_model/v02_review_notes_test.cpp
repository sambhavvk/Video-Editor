// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

TEST(V02ReviewNotesTest, NoteStoresContextAndResolve) {
  TimelineEditor editor(Project{});
  ReviewNote note;
  note.sequence_version_id = EntityId::generate();
  note.author = "sea";
  note.body = "Check eyeline on take 3";
  note.source_range = TimeRange{Time(10, 1), Time(2, 1)};
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = AddReviewNoteCommand{.note = note}}));
  EXPECT_EQ(editor.projectAt(editor.revision())->review_notes.size(), 1U);

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = ResolveReviewNoteCommand{
                                              .note_id = note.id,
                                              .resolved = true}}));
  EXPECT_TRUE(editor.projectAt(editor.revision())->review_notes.front().resolved);
}

} // namespace
} // namespace video_editor::edit
