// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace video_editor::edit {
namespace {

struct TransitionFixture final {
  Project project;
  EntityId asset_id;
  EntityId sequence_id;
  EntityId video_track_id;
  std::array<EntityId, 4> clip_ids{};
  EntityId title_clip_id;
};

[[nodiscard]] Title makeTitle(std::string text = "Title card") {
  return Title{
      .text = std::move(text),
      .font_family = "Inter",
      .font_size = 96.0,
      .foreground_color = {1.0, 1.0, 1.0, 1.0},
      .background_color = {0.0, 0.0, 0.0, 0.25},
      .horizontal_alignment = TitleHorizontalAlignment::Center,
      .bold = true,
      .italic = false,
  };
}

[[nodiscard]] Clip makeVideoClip(EntityId asset_id, std::int64_t timeline_start,
                                 std::int64_t source_start, std::int64_t duration) {
  Clip clip;
  clip.asset_id = asset_id;
  clip.kind = ClipKind::Video;
  clip.name = "Video";
  clip.timeline_range = TimeRange(Time(timeline_start, 1), Time(duration, 1));
  clip.source_range = TimeRange(Time(source_start, 1), Time(duration, 1));
  clip.playback_rate = Rate(1, 1);
  return clip;
}

[[nodiscard]] Clip makeTitleClip(std::int64_t timeline_start, std::int64_t duration) {
  Clip clip;
  clip.kind = ClipKind::Title;
  clip.name = "Title";
  clip.timeline_range = TimeRange(Time(timeline_start, 1), Time(duration, 1));
  clip.source_range = TimeRange(Time{}, Time(duration, 1));
  clip.playback_rate = Rate(1, 1);
  clip.title = makeTitle();
  return clip;
}

[[nodiscard]] TransitionFixture makeFixture(bool locked = false) {
  TransitionFixture fixture;

  Asset asset;
  asset.name = "source.mov";
  asset.source_uri = "memory://source";
  asset.duration = Time(100, 1);
  asset.has_video = true;
  asset.has_audio = true;
  asset.width = 1920;
  asset.height = 1080;
  asset.audio_sample_rate = 48'000;
  asset.audio_channels = 2;
  fixture.asset_id = asset.id;
  fixture.project.assets.push_back(asset);

  Sequence sequence;
  fixture.sequence_id = sequence.id;
  sequence.name = "Main";

  Track video;
  video.kind = TrackKind::Video;
  video.name = "V1";
  video.locked = locked;
  fixture.video_track_id = video.id;

  for (std::size_t index = 0; index < fixture.clip_ids.size(); ++index) {
    auto clip = makeVideoClip(asset.id, static_cast<std::int64_t>(index) * 10,
                              10 + static_cast<std::int64_t>(index) * 10, 10);
    fixture.clip_ids[index] = clip.id;
    video.clips.push_back(clip);
  }
  auto title = makeTitleClip(40, 10);
  fixture.title_clip_id = title.id;
  video.clips.push_back(title);

  sequence.tracks.push_back(video);
  fixture.project.sequences.push_back(sequence);
  return fixture;
}

[[nodiscard]] const Sequence& currentSequence(TimelineEditor& editor, EntityId sequence_id) {
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);
  return snapshot.value().sequence();
}

[[nodiscard]] const Clip& currentClip(TimelineEditor& editor, EntityId sequence_id,
                                      EntityId clip_id) {
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);
  const auto* clip = snapshot ? snapshot.value().findClip(clip_id) : nullptr;
  EXPECT_NE(clip, nullptr);
  return *clip;
}

[[nodiscard]] Transition makeTransition(EntityId outgoing_id, EntityId incoming_id, TimeRange range,
                                        TransitionKind kind = TransitionKind::CrossDissolve) {
  Transition transition;
  transition.outgoing_clip_id = outgoing_id;
  transition.incoming_clip_id = incoming_id;
  transition.range = range;
  transition.kind = kind;
  transition.enabled = true;
  return transition;
}

TEST(TitleTransitionTest, SetClipTitleCoalescesAndRoundTripsUndoRedo) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);

  auto first = makeTitle("First title");
  auto second = makeTitle("Second title");

  auto revision = editor.apply(
      EditCommand{SetClipTitleCommand{fixture.sequence_id, fixture.title_clip_id, first},
                  "title-gesture"},
      Revision{0});
  ASSERT_TRUE(revision) << revision.error().message;
  auto updated = editor.apply(
      EditCommand{SetClipTitleCommand{fixture.sequence_id, fixture.title_clip_id, second},
                  "title-gesture"},
      revision.value());
  ASSERT_TRUE(updated) << updated.error().message;

  ASSERT_TRUE(currentClip(editor, fixture.sequence_id, fixture.title_clip_id).title.has_value());
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.title_clip_id).title->text,
            "Second title");

  const auto history = editor.history();
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history.front().command_name, "Set clip title");
  EXPECT_EQ(history.front().command_count, 2U);

  auto undone = editor.undo(updated.value());
  ASSERT_TRUE(undone) << undone.error().message;
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.title_clip_id).title->text,
            "Title card");

  auto redone = editor.redo(undone.value());
  ASSERT_TRUE(redone) << redone.error().message;
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.title_clip_id).title->text,
            "Second title");
}

TEST(TitleTransitionTest, TitleCommandsRejectStaleLockedAndInvalidEdits) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);

  auto applied = editor.apply(
      EditCommand{SetClipTitleCommand{fixture.sequence_id, fixture.title_clip_id, makeTitle("A")},
                  {}},
      Revision{0});
  ASSERT_TRUE(applied) << applied.error().message;

  const auto stale = editor.apply(
      EditCommand{SetClipTitleCommand{fixture.sequence_id, fixture.title_clip_id, makeTitle("B")},
                  {}},
      Revision{0});
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, EditErrorCode::RevisionConflict);

  const std::array<Title, 7> invalid{
      [] {
        auto title = makeTitle();
        title.font_family.clear();
        return title;
      }(),
      [] {
        auto title = makeTitle();
        title.font_size = std::numeric_limits<double>::quiet_NaN();
        return title;
      }(),
      [] {
        auto title = makeTitle();
        title.foreground_color.alpha = 1.5;
        return title;
      }(),
      [] {
        auto title = makeTitle();
        title.text = std::string("\xFF", 1);
        return title;
      }(),
      [] {
        auto title = makeTitle();
        title.text = std::string((64U * 1024U) + 1U, 'x');
        return title;
      }(),
      [] {
        auto title = makeTitle();
        title.font_family = std::string(1025U, 'f');
        return title;
      }(),
      [] {
        auto title = makeTitle();
        title.font_size = 4097.0;
        return title;
      }(),
  };
  for (const auto& title : invalid) {
    TimelineEditor invalid_editor(fixture.project);
    const auto result = invalid_editor.apply(
        EditCommand{SetClipTitleCommand{fixture.sequence_id, fixture.title_clip_id, title}, {}},
        Revision{0});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, EditErrorCode::InvalidArgument);
    EXPECT_EQ(invalid_editor.revision(), Revision{0});
  }

  const auto wrong_kind = editor.apply(
      EditCommand{SetClipTitleCommand{fixture.sequence_id, fixture.clip_ids[0], makeTitle()}, {}},
      applied.value());
  ASSERT_FALSE(wrong_kind);
  EXPECT_EQ(wrong_kind.error().code, EditErrorCode::InvalidTrackKind);

  auto locked_fixture = makeFixture(true);
  TimelineEditor locked_editor(locked_fixture.project);
  const auto locked = locked_editor.apply(
      EditCommand{SetClipTitleCommand{locked_fixture.sequence_id, locked_fixture.title_clip_id,
                                      makeTitle("Locked")},
                  {}},
      Revision{0});
  ASSERT_FALSE(locked);
  EXPECT_EQ(locked.error().code, EditErrorCode::TrackLocked);
}

TEST(TitleTransitionTest, TransitionCommandsAddUpdateRemoveAndUndoRedo) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);

  const auto transition =
      makeTransition(fixture.clip_ids[0], fixture.clip_ids[1], TimeRange(Time(8, 1), Time(4, 1)));
  auto revision = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id, transition}, {}}, Revision{0});
  ASSERT_TRUE(revision) << revision.error().message;
  ASSERT_NE(findTransition(currentSequence(editor, fixture.sequence_id), transition.id), nullptr);

  auto updated_transition = transition;
  updated_transition.kind = TransitionKind::DipToBlack;
  auto updated = editor.apply(
      EditCommand{UpdateTransitionCommand{fixture.sequence_id, updated_transition}, {}},
      revision.value());
  ASSERT_TRUE(updated) << updated.error().message;
  ASSERT_NE(findTransition(currentSequence(editor, fixture.sequence_id), transition.id), nullptr);
  EXPECT_EQ(findTransition(currentSequence(editor, fixture.sequence_id), transition.id)->kind,
            TransitionKind::DipToBlack);

  auto removed =
      editor.apply(EditCommand{RemoveTransitionCommand{fixture.sequence_id, transition.id}, {}},
                   updated.value());
  ASSERT_TRUE(removed) << removed.error().message;
  EXPECT_EQ(currentSequence(editor, fixture.sequence_id).transitions.size(), 0U);

  const auto history = editor.history();
  ASSERT_EQ(history.size(), 3U);
  EXPECT_EQ(history[0].command_name, "Add transition");
  EXPECT_EQ(history[1].command_name, "Update transition");
  EXPECT_EQ(history[2].command_name, "Remove transition");

  auto undo_remove = editor.undo(removed.value());
  ASSERT_TRUE(undo_remove) << undo_remove.error().message;
  EXPECT_EQ(findTransition(currentSequence(editor, fixture.sequence_id), transition.id)->kind,
            TransitionKind::DipToBlack);

  auto undo_update = editor.undo(undo_remove.value());
  ASSERT_TRUE(undo_update) << undo_update.error().message;
  EXPECT_EQ(findTransition(currentSequence(editor, fixture.sequence_id), transition.id)->kind,
            TransitionKind::CrossDissolve);

  auto redo_update = editor.redo(undo_update.value());
  ASSERT_TRUE(redo_update) << redo_update.error().message;
  auto redo_remove = editor.redo(redo_update.value());
  ASSERT_TRUE(redo_remove) << redo_remove.error().message;
  EXPECT_EQ(currentSequence(editor, fixture.sequence_id).transitions.size(), 0U);
}

TEST(TitleTransitionTest, TransitionValidationRejectsInvalidPairsRangesHandlesAndOverlap) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);
  const auto expect_unmodified = [&](TimelineEditor& candidate, const Revision expected_revision) {
    EXPECT_EQ(candidate.revision(), expected_revision);
    const auto& current = currentSequence(candidate, fixture.sequence_id);
    EXPECT_TRUE(current.transitions.empty());
    EXPECT_EQ(currentClip(candidate, fixture.sequence_id, fixture.clip_ids[0]).timeline_range,
              TimeRange(Time(0, 1), Time(10, 1)));
    EXPECT_EQ(currentClip(candidate, fixture.sequence_id, fixture.clip_ids[1]).timeline_range,
              TimeRange(Time(10, 1), Time(10, 1)));
  };

  const auto duplicate_pair = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id,
                                       makeTransition(fixture.clip_ids[0], fixture.clip_ids[0],
                                                      TimeRange(Time(8, 1), Time(4, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(duplicate_pair);
  EXPECT_EQ(duplicate_pair.error().code, EditErrorCode::InvalidArgument);
  expect_unmodified(editor, Revision{0});

  const auto non_adjacent = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id,
                                       makeTransition(fixture.clip_ids[0], fixture.clip_ids[2],
                                                      TimeRange(Time(8, 1), Time(4, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(non_adjacent);
  EXPECT_EQ(non_adjacent.error().code, EditErrorCode::InvalidArgument);
  expect_unmodified(editor, Revision{0});

  const auto not_straddling = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id,
                                       makeTransition(fixture.clip_ids[0], fixture.clip_ids[1],
                                                      TimeRange(Time(10, 1), Time(4, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(not_straddling);
  EXPECT_EQ(not_straddling.error().code, EditErrorCode::InvalidArgument);
  expect_unmodified(editor, Revision{0});

  const auto extends_before_outgoing = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id,
                                       makeTransition(fixture.clip_ids[1], fixture.clip_ids[2],
                                                      TimeRange(Time(9, 1), Time(12, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(extends_before_outgoing);
  EXPECT_EQ(extends_before_outgoing.error().code, EditErrorCode::InvalidArgument);
  expect_unmodified(editor, Revision{0});

  const auto extends_after_incoming = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id,
                                       makeTransition(fixture.clip_ids[1], fixture.clip_ids[2],
                                                      TimeRange(Time(19, 1), Time(12, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(extends_after_incoming);
  EXPECT_EQ(extends_after_incoming.error().code, EditErrorCode::InvalidArgument);
  expect_unmodified(editor, Revision{0});

  auto outgoing_handle_fixture = makeFixture();
  outgoing_handle_fixture.project.sequences[0].tracks[0].clips[0].source_range =
      TimeRange(Time(89, 1), Time(10, 1));
  TimelineEditor outgoing_handle_editor(outgoing_handle_fixture.project);
  const auto outgoing_handles = outgoing_handle_editor.apply(
      EditCommand{AddTransitionCommand{outgoing_handle_fixture.sequence_id,
                                       makeTransition(outgoing_handle_fixture.clip_ids[0],
                                                      outgoing_handle_fixture.clip_ids[1],
                                                      TimeRange(Time(8, 1), Time(4, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(outgoing_handles);
  EXPECT_EQ(outgoing_handles.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(outgoing_handle_editor.revision(), Revision{0});
  EXPECT_TRUE(currentSequence(outgoing_handle_editor, outgoing_handle_fixture.sequence_id)
                  .transitions.empty());

  auto incoming_handle_fixture = makeFixture();
  incoming_handle_fixture.project.sequences[0].tracks[0].clips[1].source_range =
      TimeRange(Time(1, 1), Time(10, 1));
  TimelineEditor incoming_handle_editor(incoming_handle_fixture.project);
  const auto incoming_handles = incoming_handle_editor.apply(
      EditCommand{AddTransitionCommand{incoming_handle_fixture.sequence_id,
                                       makeTransition(incoming_handle_fixture.clip_ids[0],
                                                      incoming_handle_fixture.clip_ids[1],
                                                      TimeRange(Time(8, 1), Time(4, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(incoming_handles);
  EXPECT_EQ(incoming_handles.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(incoming_handle_editor.revision(), Revision{0});
  EXPECT_TRUE(currentSequence(incoming_handle_editor, incoming_handle_fixture.sequence_id)
                  .transitions.empty());

  auto reversed_handle_fixture = makeFixture();
  auto& reversed_outgoing = reversed_handle_fixture.project.sequences[0].tracks[0].clips[0];
  reversed_outgoing.reversed = true;
  reversed_outgoing.source_range = TimeRange(Time(1, 1), Time(10, 1));
  TimelineEditor reversed_handle_editor(reversed_handle_fixture.project);
  const auto reversed_handles = reversed_handle_editor.apply(
      EditCommand{AddTransitionCommand{reversed_handle_fixture.sequence_id,
                                       makeTransition(reversed_handle_fixture.clip_ids[0],
                                                      reversed_handle_fixture.clip_ids[1],
                                                      TimeRange(Time(8, 1), Time(4, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(reversed_handles);
  EXPECT_EQ(reversed_handles.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(reversed_handle_editor.revision(), Revision{0});
  const auto& reversed_sequence =
      currentSequence(reversed_handle_editor, reversed_handle_fixture.sequence_id);
  EXPECT_TRUE(reversed_sequence.transitions.empty());
  EXPECT_TRUE(currentClip(reversed_handle_editor, reversed_handle_fixture.sequence_id,
                          reversed_handle_fixture.clip_ids[0])
                  .reversed);
  EXPECT_EQ(currentClip(reversed_handle_editor, reversed_handle_fixture.sequence_id,
                        reversed_handle_fixture.clip_ids[0])
                .source_range,
            TimeRange(Time(1, 1), Time(10, 1)));

  const auto first = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id,
                                       makeTransition(fixture.clip_ids[0], fixture.clip_ids[1],
                                                      TimeRange(Time(8, 1), Time(12, 1)))},
                  {}},
      Revision{0});
  ASSERT_TRUE(first) << first.error().message;

  const auto overlap = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id,
                                       makeTransition(fixture.clip_ids[1], fixture.clip_ids[2],
                                                      TimeRange(Time(18, 1), Time(12, 1)))},
                  {}},
      first.value());
  ASSERT_FALSE(overlap);
  EXPECT_EQ(overlap.error().code, EditErrorCode::Overlap);

  auto locked_fixture = makeFixture(true);
  TimelineEditor locked_editor(locked_fixture.project);
  const auto locked = locked_editor.apply(
      EditCommand{AddTransitionCommand{locked_fixture.sequence_id,
                                       makeTransition(locked_fixture.clip_ids[0],
                                                      locked_fixture.clip_ids[1],
                                                      TimeRange(Time(8, 1), Time(4, 1)))},
                  {}},
      Revision{0});
  ASSERT_FALSE(locked);
  EXPECT_EQ(locked.error().code, EditErrorCode::TrackLocked);
}

TEST(TitleTransitionTest, EditsThatInvalidateTransitionsRejectAtomically) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);

  const auto transition =
      makeTransition(fixture.clip_ids[0], fixture.clip_ids[1], TimeRange(Time(8, 1), Time(4, 1)));
  auto revision = editor.apply(
      EditCommand{AddTransitionCommand{fixture.sequence_id, transition}, {}}, Revision{0});
  ASSERT_TRUE(revision) << revision.error().message;

  const auto invalid_trim =
      editor.apply(EditCommand{TrimClipCommand{fixture.sequence_id, fixture.clip_ids[1],
                                               TimeRange(Time(11, 1), Time(9, 1)),
                                               TimeRange(Time(21, 1), Time(9, 1)), false},
                               {}},
                   revision.value());
  ASSERT_FALSE(invalid_trim);
  EXPECT_EQ(invalid_trim.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.revision(), revision.value());

  const auto& sequence = currentSequence(editor, fixture.sequence_id);
  EXPECT_EQ(findTransition(sequence, transition.id)->range, transition.range);
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.clip_ids[1]).timeline_range.start,
            Time(10, 1));
}

TEST(TitleTransitionTest, SetClipSpeedUpdatesRateAndReversed) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);

  const auto before = currentClip(editor, fixture.sequence_id, fixture.clip_ids[0]);
  EXPECT_EQ(before.playback_rate, Rate(1, 1));
  EXPECT_EQ(before.reversed, false);

  SetClipSpeedCommand command;
  command.sequence_id = fixture.sequence_id;
  command.clip_id = fixture.clip_ids[0];
  command.playback_rate = Rate(2, 1);
  command.reversed = true;

  const auto revision = editor.apply({command, ""}, Revision{});
  ASSERT_TRUE(revision) << revision.error().message;

  const auto& clip = currentClip(editor, fixture.sequence_id, fixture.clip_ids[0]);
  EXPECT_EQ(clip.playback_rate, Rate(2, 1));
  EXPECT_EQ(clip.reversed, true);
}

TEST(TitleTransitionTest, SetClipSpeedRejectsTitleClip) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);

  SetClipSpeedCommand command;
  command.sequence_id = fixture.sequence_id;
  command.clip_id = fixture.title_clip_id;
  command.playback_rate = Rate(2, 1);
  command.reversed = true;

  const auto result = editor.apply({command, ""}, Revision{});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, EditErrorCode::InvalidTrackKind);
  EXPECT_EQ(editor.revision(), Revision{});
}

TEST(TitleTransitionTest, SetClipSpeedRejectsLockedTrack) {
  auto fixture = makeFixture(true);
  TimelineEditor editor(fixture.project);

  SetClipSpeedCommand command;
  command.sequence_id = fixture.sequence_id;
  command.clip_id = fixture.clip_ids[0];
  command.playback_rate = Rate(2, 1);
  command.reversed = true;

  const auto result = editor.apply({command, ""}, Revision{});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, EditErrorCode::TrackLocked);
  EXPECT_EQ(editor.revision(), Revision{});
}

TEST(TitleTransitionTest, SetClipSpeedRejectsZeroDenominator) {
  EXPECT_THROW(Rate(1, 0), std::invalid_argument);
}

TEST(TitleTransitionTest, SetClipSpeedRejectsNonPositiveNumerator) {
  EXPECT_THROW(Rate(0, 1), std::invalid_argument);
}

TEST(TitleTransitionTest, SetClipSpeedRejectsOutOfRangeRate) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);

  SetClipSpeedCommand command;
  command.sequence_id = fixture.sequence_id;
  command.clip_id = fixture.clip_ids[0];

  command.playback_rate = Rate(1, 1000);
  command.reversed = false;
  auto result = editor.apply({command, ""}, Revision{});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.revision(), Revision{});

  command.playback_rate = Rate(1001, 1);
  command.reversed = false;
  result = editor.apply({command, ""}, Revision{});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.revision(), Revision{});
}

TEST(TitleTransitionTest, SetClipSpeedAcceptsBoundaryRates) {
  auto fixture = makeFixture();
  TimelineEditor editor(fixture.project);

  SetClipSpeedCommand command;
  command.sequence_id = fixture.sequence_id;
  command.clip_id = fixture.clip_ids[0];

  command.playback_rate = Rate(1, 100);
  command.reversed = false;
  auto result = editor.apply({command, ""}, Revision{});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.clip_ids[0]).playback_rate,
            Rate(1, 100));

  command.playback_rate = Rate(100, 1);
  command.reversed = true;
  result = editor.apply({command, ""}, result.value());
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.clip_ids[0]).playback_rate,
            Rate(100, 1));
}

} // namespace
} // namespace video_editor::edit
