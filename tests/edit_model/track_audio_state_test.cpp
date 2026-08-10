// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

#include <variant>

namespace video_editor::edit {
namespace {

struct MixerFixture final {
  Project project;
  EntityId sequence_id;
  EntityId audio_track_id;
  EntityId video_track_id;
};

[[nodiscard]] MixerFixture makeMixerFixture(const bool audio_track_locked = false) {
  MixerFixture result;
  result.project.name = "Mixer state fixture";
  result.project.metadata.emplace("unrelated", "preserved");

  Sequence sequence;
  sequence.name = "Main sequence";
  result.sequence_id = sequence.id;

  Track audio;
  audio.kind = TrackKind::Audio;
  audio.name = "Dialogue";
  audio.locked = audio_track_locked;
  audio.muted = false;
  audio.solo = false;
  result.audio_track_id = audio.id;

  Track video;
  video.kind = TrackKind::Video;
  video.name = "Picture";
  result.video_track_id = video.id;

  sequence.tracks = {audio, video};
  result.project.sequences.push_back(sequence);
  return result;
}

[[nodiscard]] const Track& currentTrack(TimelineEditor& editor, const EntityId sequence_id,
                                        const EntityId track_id) {
  const auto current = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(current) << (current ? "" : current.error().message);
  const auto* track = current ? current.value().findTrack(track_id) : nullptr;
  EXPECT_NE(track, nullptr);
  return *track;
}

TEST(TrackAudioStateTest, AppliesToLockedAudioTrackAndPreservesEveryOtherProjectField) {
  auto fixture = makeMixerFixture(true);
  const Project initial = fixture.project;
  Project expected = initial;
  expected.sequences.front().tracks.front().muted = true;
  expected.sequences.front().tracks.front().solo = true;
  TimelineEditor editor(initial);

  const EditCommand command{
      SetTrackAudioStateCommand{fixture.sequence_id, fixture.audio_track_id, true, true}, {}};
  EXPECT_EQ(commandName(command), "Set track audio state");
  const auto applied = editor.apply(command, Revision{0});

  ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);
  EXPECT_EQ(applied.value(), Revision{1});
  EXPECT_EQ(*editor.projectAt(applied.value()), expected);
  const auto& track = currentTrack(editor, fixture.sequence_id, fixture.audio_track_id);
  EXPECT_TRUE(track.locked);
  EXPECT_TRUE(track.muted);
  EXPECT_TRUE(track.solo);

  const auto undone = editor.undo(applied.value());
  ASSERT_TRUE(undone);
  EXPECT_EQ(*editor.projectAt(undone.value()), initial);
  const auto redone = editor.redo(undone.value());
  ASSERT_TRUE(redone);
  EXPECT_EQ(*editor.projectAt(redone.value()), expected);
}

TEST(TrackAudioStateTest, CoalescesAdjacentMixerUpdatesIntoOneUndoStep) {
  auto fixture = makeMixerFixture();
  const Project initial = fixture.project;
  TimelineEditor editor(initial);

  auto revision =
      editor.apply(EditCommand{SetTrackAudioStateCommand{fixture.sequence_id,
                                                         fixture.audio_track_id, true, false},
                               "audio-mixer-dialogue"},
                   Revision{0});
  ASSERT_TRUE(revision);
  revision = editor.apply(EditCommand{SetTrackAudioStateCommand{
                                          fixture.sequence_id, fixture.audio_track_id, false, true},
                                      "audio-mixer-dialogue"},
                          revision.value());
  ASSERT_TRUE(revision);

  const auto history = editor.history();
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history.front().command_name, "Set track audio state");
  EXPECT_EQ(history.front().coalescing_key, "audio-mixer-dialogue");
  EXPECT_EQ(history.front().command_count, 2U);
  const auto& changed = currentTrack(editor, fixture.sequence_id, fixture.audio_track_id);
  EXPECT_FALSE(changed.muted);
  EXPECT_TRUE(changed.solo);

  const auto undone = editor.undo(revision.value());
  ASSERT_TRUE(undone);
  EXPECT_EQ(*editor.projectAt(undone.value()), initial);
  EXPECT_FALSE(editor.canUndo());
  EXPECT_TRUE(editor.canRedo());
}

TEST(TrackAudioStateTest, RejectsStaleMissingAndNonAudioTargetsAtomically) {
  auto fixture = makeMixerFixture();
  TimelineEditor editor(fixture.project);

  const EditCommand valid{
      SetTrackAudioStateCommand{fixture.sequence_id, fixture.audio_track_id, true, false}, {}};
  const auto stale = editor.apply(valid, Revision{1});
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, EditErrorCode::RevisionConflict);
  EXPECT_EQ(stale.error().expected_revision, Revision{1});
  EXPECT_EQ(stale.error().actual_revision, Revision{0});

  const EditCommand missing_sequence{
      SetTrackAudioStateCommand{EntityId::generate(), fixture.audio_track_id, true, false}, {}};
  const auto no_sequence = editor.apply(missing_sequence, Revision{0});
  ASSERT_FALSE(no_sequence);
  EXPECT_EQ(no_sequence.error().code, EditErrorCode::EntityNotFound);

  const EditCommand missing_track{
      SetTrackAudioStateCommand{fixture.sequence_id, EntityId::generate(), true, false}, {}};
  const auto no_track = editor.apply(missing_track, Revision{0});
  ASSERT_FALSE(no_track);
  EXPECT_EQ(no_track.error().code, EditErrorCode::EntityNotFound);

  const EditCommand video_track{
      SetTrackAudioStateCommand{fixture.sequence_id, fixture.video_track_id, true, true}, {}};
  const auto wrong_kind = editor.apply(video_track, Revision{0});
  ASSERT_FALSE(wrong_kind);
  EXPECT_EQ(wrong_kind.error().code, EditErrorCode::InvalidTrackKind);

  EXPECT_EQ(editor.revision(), Revision{0});
  EXPECT_EQ(*editor.projectAt(Revision{0}), fixture.project);
  EXPECT_FALSE(editor.canUndo());
  EXPECT_TRUE(editor.history().empty());
}

TEST(TrackAudioStateTest, IsAppendedAfterEveryExistingOperation) {
  EXPECT_EQ(EditOperation{SetClipAudioPropertiesCommand{}}.index(), 26U);
  EXPECT_EQ(EditOperation{SetTrackAudioStateCommand{}}.index(), 27U);
  EXPECT_EQ(std::variant_size_v<EditOperation>, 28U);
}

} // namespace
} // namespace video_editor::edit
