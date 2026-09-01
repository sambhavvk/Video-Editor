// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

namespace video_editor::edit {
namespace {

struct TestProject final {
  Project project;
  EntityId asset_id;
  EntityId sequence_id;
  EntityId video_track_id;
  EntityId audio_track_id;
};

[[nodiscard]] TestProject makeProject() {
  TestProject result;
  result.project.name = "Core test";

  Asset asset;
  asset.name = "source.mov";
  asset.source_uri = "/media/source.mov";
  asset.duration = Time(120, 1);
  asset.has_video = true;
  asset.has_audio = true;
  asset.width = 3840;
  asset.height = 2160;
  asset.nominal_frame_rate = Rate(30'000, 1'001);
  asset.audio_sample_rate = 48'000;
  asset.audio_channels = 2;
  result.asset_id = asset.id;
  result.project.assets.push_back(asset);

  Sequence sequence;
  sequence.name = "Main";
  sequence.frame_rate = Rate(30'000, 1'001);
  result.sequence_id = sequence.id;

  Track video;
  video.kind = TrackKind::Video;
  video.name = "V1";
  result.video_track_id = video.id;
  sequence.tracks.push_back(video);

  Track audio;
  audio.kind = TrackKind::Audio;
  audio.name = "A1";
  result.audio_track_id = audio.id;
  sequence.tracks.push_back(audio);

  result.project.sequences.push_back(sequence);
  return result;
}

[[nodiscard]] Clip makeClip(EntityId asset_id, std::int64_t start, std::int64_t duration,
                            ClipKind kind = ClipKind::Video) {
  Clip clip;
  clip.asset_id = asset_id;
  clip.kind = kind;
  clip.name = kind == ClipKind::Audio ? "Audio" : "Video";
  clip.timeline_range = TimeRange(Time(start, 1), Time(duration, 1));
  clip.source_range = TimeRange(Time(0, 1), Time(duration, 1));
  return clip;
}

[[nodiscard]] Revision insert(TimelineEditor& editor, Revision revision, EntityId sequence_id,
                              EntityId track_id, const Clip& clip,
                              InsertMode mode = InsertMode::RejectOverlap) {
  auto result =
      editor.apply(EditCommand{InsertClipCommand{sequence_id, track_id, clip, mode}, {}}, revision);
  EXPECT_TRUE(result) << (result ? "" : result.error().message);
  return result ? result.value() : revision;
}

[[nodiscard]] TimelineSnapshot snapshot(TimelineEditor& editor, EntityId sequence_id,
                                        Revision revision) {
  auto result = editor.snapshot(sequence_id, revision);
  EXPECT_TRUE(result) << (result ? "" : result.error().message);
  return result ? result.value() : TimelineSnapshot{};
}

TEST(TimelineEditorTest, RejectsStaleCommandsAndKeepsOldSnapshotsImmutable) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const auto original = snapshot(editor, fixture.sequence_id, Revision{0});
  EXPECT_EQ(original.findTrack(fixture.video_track_id)->clips.size(), 0U);

  const auto clip = makeClip(fixture.asset_id, 0, 10);
  const auto revision =
      insert(editor, Revision{0}, fixture.sequence_id, fixture.video_track_id, clip);
  EXPECT_EQ(revision, Revision{1});
  EXPECT_EQ(original.findTrack(fixture.video_track_id)->clips.size(), 0U);
  EXPECT_NE(snapshot(editor, fixture.sequence_id, revision).findClip(clip.id), nullptr);

  Marker marker;
  marker.range = TimeRange(Time(1, 1), Time{});
  const auto stale =
      editor.apply(EditCommand{AddMarkerCommand{fixture.sequence_id, marker}, {}}, Revision{0});
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, EditErrorCode::RevisionConflict);
  EXPECT_EQ(stale.error().expected_revision, Revision{0});
  EXPECT_EQ(stale.error().actual_revision, Revision{1});
  EXPECT_EQ(editor.revision(), Revision{1});
}

TEST(TimelineEditorTest, BatchIsAtomicAndUndoRedoIsOneHistoryStep) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  auto first = makeClip(fixture.asset_id, 0, 10);
  auto second = makeClip(fixture.asset_id, 10, 10);
  const auto applied = editor.applyBatch(
      {EditCommand{InsertClipCommand{fixture.sequence_id, fixture.video_track_id, first}, {}},
       EditCommand{InsertClipCommand{fixture.sequence_id, fixture.video_track_id, second}, {}}},
      Revision{0}, "Insert pair", std::string{"pair"});
  ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);
  EXPECT_EQ(applied.value(), Revision{1});
  ASSERT_EQ(editor.history().size(), 1U);
  EXPECT_EQ(editor.history().front().command_name, "Insert pair");
  EXPECT_EQ(editor.history().front().command_count, 2U);
  const auto undone = editor.undo(applied.value());
  ASSERT_TRUE(undone);
  EXPECT_EQ(snapshot(editor, fixture.sequence_id, undone.value())
                .findTrack(fixture.video_track_id)
                ->clips.size(),
            0U);
  const auto redone = editor.redo(undone.value());
  ASSERT_TRUE(redone);
  EXPECT_EQ(snapshot(editor, fixture.sequence_id, redone.value())
                .findTrack(fixture.video_track_id)
                ->clips.size(),
            2U);

  const auto failed =
      editor.applyBatch({EditCommand{InsertClipCommand{fixture.sequence_id, fixture.video_track_id,
                                                       makeClip(fixture.asset_id, 20, 10)},
                                     {}},
                         EditCommand{InsertClipCommand{fixture.sequence_id, fixture.video_track_id,
                                                       makeClip(fixture.asset_id, 25, 10)},
                                     {}}},
                        redone.value(), "Must fail");
  ASSERT_FALSE(failed);
  EXPECT_EQ(editor.revision(), redone.value());
  EXPECT_EQ(snapshot(editor, fixture.sequence_id, redone.value())
                .findTrack(fixture.video_track_id)
                ->clips.size(),
            2U);
}

TEST(TimelineEditorTest, SplitsLinkedCompanionsWithExplicitRightIds) {
  auto fixture = makeProject();
  auto video = makeClip(fixture.asset_id, 0, 10);
  auto audio = makeClip(fixture.asset_id, 0, 10, ClipKind::Audio);
  const auto group = EntityId::generate();
  video.linked_group = group;
  audio.linked_group = group;
  fixture.project.sequences[0].tracks[0].clips.push_back(video);
  fixture.project.sequences[0].tracks[1].clips.push_back(audio);
  TimelineEditor editor(fixture.project);
  const auto video_right = EntityId::generate();
  const auto audio_right = EntityId::generate();
  const auto applied = editor.apply(EditCommand{SplitClipCommand{fixture.sequence_id,
                                                                 video.id,
                                                                 Time(5, 1),
                                                                 video_right,
                                                                 true,
                                                                 {{audio.id, audio_right}}},
                                                {}},
                                    Revision{0});
  ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);
  const auto current = snapshot(editor, fixture.sequence_id, applied.value());
  ASSERT_NE(current.findClip(video_right), nullptr);
  ASSERT_NE(current.findClip(audio_right), nullptr);
  EXPECT_EQ(current.findClip(audio_right)->timeline_range.start, Time(5, 1));
}

TEST(TimelineEditorTest, TrackStateAndCloseGapRejectStaleRange) {
  auto fixture = makeProject();
  auto first = makeClip(fixture.asset_id, 0, 5);
  auto second = makeClip(fixture.asset_id, 10, 5);
  fixture.project.sequences[0].tracks[0].clips = {first, second};
  TimelineEditor editor(fixture.project);
  auto revision = editor.apply(
      EditCommand{RenameTrackCommand{fixture.sequence_id, fixture.video_track_id, "Picture"}, {}},
      Revision{0});
  ASSERT_TRUE(revision);
  revision = editor.apply(
      EditCommand{SetTrackVisibilityCommand{fixture.sequence_id, fixture.video_track_id, false},
                  {}},
      revision.value());
  ASSERT_TRUE(revision);
  revision = editor.apply(
      EditCommand{SetTrackTargetedCommand{fixture.sequence_id, fixture.video_track_id, false}, {}},
      revision.value());
  ASSERT_TRUE(revision);
  revision = editor.apply(EditCommand{CloseGapCommand{fixture.sequence_id, fixture.video_track_id,
                                                      TimeRange(Time(5, 1), Time(5, 1))},
                                      {}},
                          revision.value());
  ASSERT_TRUE(revision);
  const auto current = snapshot(editor, fixture.sequence_id, revision.value());
  EXPECT_EQ(current.findClip(second.id)->timeline_range.start, Time(5, 1));
  EXPECT_FALSE(current.findTrack(fixture.video_track_id)->visible);
  EXPECT_FALSE(current.findTrack(fixture.video_track_id)->targeted);
  const auto stale =
      editor.apply(EditCommand{CloseGapCommand{fixture.sequence_id, fixture.video_track_id,
                                               TimeRange(Time(5, 1), Time(5, 1))},
                               {}},
                   revision.value());
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, EditErrorCode::InvalidArgument);
}

TEST(TimelineEditorTest, RippleTrimInAndOutKeepAdjacentMaterialContiguous) {
  auto fixture = makeProject();
  auto first = makeClip(fixture.asset_id, 10, 10);
  auto second = makeClip(fixture.asset_id, 20, 10);
  fixture.project.sequences[0].tracks[0].clips = {first, second};
  TimelineEditor editor(fixture.project);
  auto revision = editor.apply(
      EditCommand{TrimClipCommand{fixture.sequence_id, first.id, TimeRange(Time(12, 1), Time(8, 1)),
                                  TimeRange(Time(2, 1), Time(8, 1)), false, InsertMode::Ripple},
                  {}},
      Revision{0});
  ASSERT_TRUE(revision) << (revision ? "" : revision.error().message);
  auto current = snapshot(editor, fixture.sequence_id, revision.value());
  EXPECT_EQ(current.findClip(first.id)->timeline_range, TimeRange(Time(10, 1), Time(8, 1)));
  EXPECT_EQ(current.findClip(second.id)->timeline_range.start, Time(18, 1));

  revision = editor.apply(
      EditCommand{TrimClipCommand{fixture.sequence_id, first.id, TimeRange(Time(10, 1), Time(6, 1)),
                                  TimeRange(Time(2, 1), Time(6, 1)), false, InsertMode::Ripple},
                  {}},
      revision.value());
  ASSERT_TRUE(revision) << (revision ? "" : revision.error().message);
  current = snapshot(editor, fixture.sequence_id, revision.value());
  EXPECT_EQ(current.findClip(first.id)->timeline_range.end(), Time(16, 1));
  EXPECT_EQ(current.findClip(second.id)->timeline_range.start, Time(16, 1));
}

TEST(TimelineEditorTest, CloseGapRejectsTerminalGapWithoutMutation) {
  auto fixture = makeProject();
  auto clip = makeClip(fixture.asset_id, 0, 5);
  fixture.project.sequences[0].tracks[0].clips = {clip};
  fixture.project.sequences[0].tracks[1].clips = {
      makeClip(fixture.asset_id, 0, 10, ClipKind::Audio)};
  TimelineEditor editor(fixture.project);
  const auto rejected =
      editor.apply(EditCommand{CloseGapCommand{fixture.sequence_id, fixture.video_track_id,
                                               TimeRange(Time(5, 1), Time(5, 1))},
                               {}},
                   Revision{0});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(editor.revision(), Revision{0});
}

TEST(TimelineEditorTest, SetsSequenceFormatAsOneExactRevisionAndRoundTripsHistory) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const auto before = snapshot(editor, fixture.sequence_id, Revision{0});

  const SetSequenceFormatCommand format{
      fixture.sequence_id,
      Rate(24'000, 1'001),
      4096,
      2160,
  };
  const auto applied = editor.apply(EditCommand{format, {}}, Revision{0});
  ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);
  EXPECT_EQ(applied.value(), Revision{1});

  const auto current = snapshot(editor, fixture.sequence_id, applied.value());
  EXPECT_EQ(current.sequence().frame_rate, Rate(24'000, 1'001));
  EXPECT_EQ(current.sequence().width, 4096U);
  EXPECT_EQ(current.sequence().height, 2160U);
  EXPECT_EQ(before.sequence().frame_rate, Rate(30'000, 1'001));
  EXPECT_EQ(before.sequence().width, 1920U);
  EXPECT_EQ(before.sequence().height, 1080U);

  const auto history = editor.history();
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history.front().command_name, "Set sequence format");

  const auto undone = editor.undo(applied.value());
  ASSERT_TRUE(undone) << (undone ? "" : undone.error().message);
  const auto after_undo = snapshot(editor, fixture.sequence_id, undone.value());
  EXPECT_EQ(after_undo.sequence().frame_rate, Rate(30'000, 1'001));
  EXPECT_EQ(after_undo.sequence().width, 1920U);
  EXPECT_EQ(after_undo.sequence().height, 1080U);

  const auto redone = editor.redo(undone.value());
  ASSERT_TRUE(redone) << (redone ? "" : redone.error().message);
  const auto after_redo = snapshot(editor, fixture.sequence_id, redone.value());
  EXPECT_EQ(after_redo.sequence().frame_rate, Rate(24'000, 1'001));
  EXPECT_EQ(after_redo.sequence().width, 4096U);
  EXPECT_EQ(after_redo.sequence().height, 2160U);
}

TEST(TimelineEditorTest, RejectsInvalidSequenceFormatAtomically) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);

  const auto zero_width = editor.apply(
      EditCommand{SetSequenceFormatCommand{fixture.sequence_id, Rate(60, 1), 0, 1080}, {}},
      Revision{0});
  ASSERT_FALSE(zero_width);
  EXPECT_EQ(zero_width.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.revision(), Revision{0});
  EXPECT_FALSE(editor.canUndo());

  const auto zero_height = editor.apply(
      EditCommand{SetSequenceFormatCommand{fixture.sequence_id, Rate(60, 1), 1920, 0}, {}},
      Revision{0});
  ASSERT_FALSE(zero_height);
  EXPECT_EQ(zero_height.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.revision(), Revision{0});
  EXPECT_FALSE(editor.canUndo());

  const auto current = snapshot(editor, fixture.sequence_id, Revision{0});
  EXPECT_EQ(current.sequence(), fixture.project.sequences.front());
}

TEST(TimelineEditorTest, SequenceFormatHonorsEntityAndRevisionContracts) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const SetSequenceFormatCommand format{fixture.sequence_id, Rate(25, 1), 1280, 720};

  const auto stale = editor.apply(EditCommand{format, {}}, Revision{1});
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, EditErrorCode::RevisionConflict);

  const auto missing = editor.apply(
      EditCommand{SetSequenceFormatCommand{EntityId::generate(), Rate(25, 1), 1280, 720}, {}},
      Revision{0});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, EditErrorCode::EntityNotFound);
  EXPECT_EQ(editor.revision(), Revision{0});
  EXPECT_FALSE(editor.canUndo());
}

TEST(TimelineEditorTest, RejectsOverlapAndWrongTrackKindWithoutMutation) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const auto first = makeClip(fixture.asset_id, 0, 10);
  auto revision = insert(editor, Revision{0}, fixture.sequence_id, fixture.video_track_id, first);

  const auto overlapping = makeClip(fixture.asset_id, 5, 3);
  const auto rejected =
      editor.apply(EditCommand{InsertClipCommand{fixture.sequence_id, fixture.video_track_id,
                                                 overlapping, InsertMode::RejectOverlap},
                               {}},
                   revision);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, EditErrorCode::Overlap);
  EXPECT_EQ(editor.revision(), revision);

  const auto audio = makeClip(fixture.asset_id, 11, 3, ClipKind::Audio);
  const auto wrong_track =
      editor.apply(EditCommand{InsertClipCommand{fixture.sequence_id, fixture.video_track_id, audio,
                                                 InsertMode::RejectOverlap},
                               {}},
                   revision);
  ASSERT_FALSE(wrong_track);
  EXPECT_EQ(wrong_track.error().code, EditErrorCode::InvalidTrackKind);
}

TEST(TimelineEditorTest, SplitsClipAndMapsSourceRanges) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const auto clip = makeClip(fixture.asset_id, 10, 10);
  auto revision = insert(editor, Revision{0}, fixture.sequence_id, fixture.video_track_id, clip);

  const auto right_id = EntityId::generate();
  auto result = editor.apply(
      EditCommand{SplitClipCommand{fixture.sequence_id, clip.id, Time(14, 1), right_id}, {}},
      revision);
  ASSERT_TRUE(result) << (result ? "" : result.error().message);
  revision = result.value();

  const auto current = snapshot(editor, fixture.sequence_id, revision);
  const auto* left = current.findClip(clip.id);
  const auto* right = current.findClip(right_id);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->timeline_range, TimeRange(Time(10, 1), Time(4, 1)));
  EXPECT_EQ(left->source_range, TimeRange(Time(0, 1), Time(4, 1)));
  EXPECT_EQ(right->timeline_range, TimeRange(Time(14, 1), Time(6, 1)));
  EXPECT_EQ(right->source_range, TimeRange(Time(4, 1), Time(6, 1)));
  EXPECT_EQ(left->name, "Video[10:14]");
  EXPECT_EQ(right->name, "Video[14:20]");
}

TEST(TimelineEditorTest, SplitRenamesHalvesWithWholeSecondRanges) {
  auto fixture = makeProject();
  fixture.project.assets[0].duration = Time(600, 1);
  TimelineEditor editor(fixture.project);
  auto clip = makeClip(fixture.asset_id, 0, 600);
  clip.name = "clip1[0:130]";
  clip.source_range = TimeRange(Time(0, 1), Time(600, 1));
  auto revision = insert(editor, Revision{0}, fixture.sequence_id, fixture.video_track_id, clip);

  const auto right_id = EntityId::generate();
  auto result = editor.apply(
      EditCommand{SplitClipCommand{fixture.sequence_id, clip.id, Time(130, 1), right_id}, {}},
      revision);
  ASSERT_TRUE(result) << (result ? "" : result.error().message);

  const auto current = snapshot(editor, fixture.sequence_id, result.value());
  const auto* left = current.findClip(clip.id);
  const auto* right = current.findClip(right_id);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->name, "clip1[0:130]");
  EXPECT_EQ(right->name, "clip1[130:600]");
}

TEST(TimelineEditorTest, SplitRenamesLinkedCompanions) {
  auto fixture = makeProject();
  auto video = makeClip(fixture.asset_id, 0, 10);
  auto audio = makeClip(fixture.asset_id, 0, 10, ClipKind::Audio);
  video.name = "Scene";
  audio.name = "Scene";
  const auto group = EntityId::generate();
  video.linked_group = group;
  audio.linked_group = group;
  fixture.project.sequences[0].tracks[0].clips.push_back(video);
  fixture.project.sequences[0].tracks[1].clips.push_back(audio);
  TimelineEditor editor(fixture.project);
  const auto video_right = EntityId::generate();
  const auto audio_right = EntityId::generate();
  const auto applied = editor.apply(EditCommand{SplitClipCommand{fixture.sequence_id,
                                                                 video.id,
                                                                 Time(5, 1),
                                                                 video_right,
                                                                 true,
                                                                 {{audio.id, audio_right}}},
                                                {}},
                                    Revision{0});
  ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);
  const auto current = snapshot(editor, fixture.sequence_id, applied.value());
  EXPECT_EQ(current.findClip(video.id)->name, "Scene[0:5]");
  EXPECT_EQ(current.findClip(video_right)->name, "Scene[5:10]");
  EXPECT_EQ(current.findClip(audio.id)->name, "Scene[0:5]");
  EXPECT_EQ(current.findClip(audio_right)->name, "Scene[5:10]");
}

TEST(TimelineEditorTest, OverwritePreservesUncoveredClipSegments) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const auto base = makeClip(fixture.asset_id, 0, 10);
  auto revision = insert(editor, Revision{0}, fixture.sequence_id, fixture.video_track_id, base);
  auto overlay = makeClip(fixture.asset_id, 3, 4);
  overlay.source_range.start = Time(20, 1);
  revision = insert(editor, revision, fixture.sequence_id, fixture.video_track_id, overlay,
                    InsertMode::Overwrite);

  const auto* track =
      snapshot(editor, fixture.sequence_id, revision).findTrack(fixture.video_track_id);
  ASSERT_NE(track, nullptr);
  ASSERT_EQ(track->clips.size(), 3U);
  EXPECT_EQ(track->clips[0].timeline_range, TimeRange(Time(0, 1), Time(3, 1)));
  EXPECT_EQ(track->clips[1].timeline_range, TimeRange(Time(3, 1), Time(4, 1)));
  EXPECT_EQ(track->clips[1].id, overlay.id);
  EXPECT_EQ(track->clips[2].timeline_range, TimeRange(Time(7, 1), Time(3, 1)));
  EXPECT_EQ(track->clips[2].source_range, TimeRange(Time(7, 1), Time(3, 1)));
}

TEST(TimelineEditorTest, RippleRemovalClosesOnlyTheTargetTrackGap) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const auto first = makeClip(fixture.asset_id, 0, 2);
  const auto middle = makeClip(fixture.asset_id, 2, 2);
  const auto last = makeClip(fixture.asset_id, 4, 2);
  auto revision = insert(editor, Revision{0}, fixture.sequence_id, fixture.video_track_id, first);
  revision = insert(editor, revision, fixture.sequence_id, fixture.video_track_id, middle);
  revision = insert(editor, revision, fixture.sequence_id, fixture.video_track_id, last);

  auto removed = editor.apply(
      EditCommand{RemoveClipCommand{fixture.sequence_id, middle.id, true}, {}}, revision);
  ASSERT_TRUE(removed) << (removed ? "" : removed.error().message);
  const auto current = snapshot(editor, fixture.sequence_id, removed.value());
  EXPECT_EQ(current.findClip(last.id)->timeline_range.start, Time(2, 1));
  EXPECT_EQ(current.duration(), Time(4, 1));
}

TEST(TimelineEditorTest, CoalescedCommandsUndoAndRedoAsOneGesture) {
  auto fixture = makeProject();
  auto clip = makeClip(fixture.asset_id, 0, 10);
  fixture.project.sequences[0].tracks[0].clips.push_back(clip);
  TimelineEditor editor(fixture.project);

  auto first_trim = editor.apply(
      EditCommand{TrimClipCommand{fixture.sequence_id, clip.id, TimeRange(Time{}, Time(9, 1)),
                                  TimeRange(Time{}, Time(9, 1))},
                  "trim-gesture-1"},
      Revision{0});
  ASSERT_TRUE(first_trim);
  auto second_trim = editor.apply(
      EditCommand{TrimClipCommand{fixture.sequence_id, clip.id, TimeRange(Time{}, Time(8, 1)),
                                  TimeRange(Time{}, Time(8, 1))},
                  "trim-gesture-1"},
      first_trim.value());
  ASSERT_TRUE(second_trim);

  const auto entries = editor.history();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].command_count, 2U);
  EXPECT_EQ(entries[0].command_name, "Trim clip");

  auto undone = editor.undo(second_trim.value());
  ASSERT_TRUE(undone);
  EXPECT_EQ(snapshot(editor, fixture.sequence_id, undone.value())
                .findClip(clip.id)
                ->timeline_range.duration,
            Time(10, 1));
  EXPECT_TRUE(editor.canRedo());

  auto redone = editor.redo(undone.value());
  ASSERT_TRUE(redone);
  EXPECT_EQ(snapshot(editor, fixture.sequence_id, redone.value())
                .findClip(clip.id)
                ->timeline_range.duration,
            Time(8, 1));
}

TEST(TimelineEditorTest, NewEditAfterUndoDiscardsRedoBranch) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  Marker first;
  first.range = TimeRange(Time(1, 1), Time{});
  auto added =
      editor.apply(EditCommand{AddMarkerCommand{fixture.sequence_id, first}, {}}, Revision{0});
  ASSERT_TRUE(added);
  auto undone = editor.undo(added.value());
  ASSERT_TRUE(undone);

  Marker replacement;
  replacement.range = TimeRange(Time(2, 1), Time{});
  auto branched = editor.apply(EditCommand{AddMarkerCommand{fixture.sequence_id, replacement}, {}},
                               undone.value());
  ASSERT_TRUE(branched);
  EXPECT_FALSE(editor.canRedo());
  const auto redo = editor.redo(branched.value());
  ASSERT_FALSE(redo);
  EXPECT_EQ(redo.error().code, EditErrorCode::NothingToRedo);
}

TEST(TimelineEditorTest, DerivesStableGapsFromAnImmutableRevision) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const auto first = makeClip(fixture.asset_id, 2, 2);
  const auto second = makeClip(fixture.asset_id, 6, 2);
  auto revision = insert(editor, Revision{0}, fixture.sequence_id, fixture.video_track_id, first);
  revision = insert(editor, revision, fixture.sequence_id, fixture.video_track_id, second);
  const auto before_more_edits = snapshot(editor, fixture.sequence_id, revision);

  const auto gaps = before_more_edits.gaps(fixture.video_track_id, Time(10, 1));
  ASSERT_EQ(gaps.size(), 3U);
  EXPECT_EQ(gaps[0].timeline_range, TimeRange(Time(0, 1), Time(2, 1)));
  EXPECT_EQ(gaps[1].timeline_range, TimeRange(Time(4, 1), Time(2, 1)));
  EXPECT_EQ(gaps[2].timeline_range, TimeRange(Time(8, 1), Time(2, 1)));

  const auto third = makeClip(fixture.asset_id, 8, 1);
  revision = insert(editor, revision, fixture.sequence_id, fixture.video_track_id, third);
  EXPECT_EQ(before_more_edits.gaps(fixture.video_track_id, Time(10, 1)).size(), 3U);
  EXPECT_EQ(snapshot(editor, fixture.sequence_id, revision)
                .gaps(fixture.video_track_id, Time(10, 1))
                .size(),
            3U);
}

TEST(TimelineEditorTest, RoundTripsUnknownEffectsMarkersAndCaptions) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  const auto clip = makeClip(fixture.asset_id, 0, 10);
  auto revision = insert(editor, Revision{0}, fixture.sequence_id, fixture.video_track_id, clip);

  Effect unknown;
  unknown.type = "future.vendor.effect";
  unknown.version = 42;
  unknown.enabled = false;
  unknown.known = false;
  unknown.opaque_payload = {0x01, 0xA5, 0xFF};
  auto effect_result = editor.apply(
      EditCommand{AddClipEffectCommand{fixture.sequence_id, clip.id, unknown}, {}}, revision);
  ASSERT_TRUE(effect_result);
  revision = effect_result.value();

  Marker marker;
  marker.range = TimeRange(Time(2, 1), Time{});
  marker.label = "Beat";
  auto marker_result =
      editor.apply(EditCommand{AddMarkerCommand{fixture.sequence_id, marker}, {}}, revision);
  ASSERT_TRUE(marker_result);
  revision = marker_result.value();

  Caption caption;
  caption.range = TimeRange(Time(1, 1), Time(3, 1));
  caption.text = "Hello world";
  caption.language = "en";
  auto caption_result =
      editor.apply(EditCommand{AddCaptionCommand{fixture.sequence_id, caption}, {}}, revision);
  ASSERT_TRUE(caption_result);

  const auto current = snapshot(editor, fixture.sequence_id, caption_result.value());
  ASSERT_EQ(current.findClip(clip.id)->effects.size(), 1U);
  EXPECT_EQ(current.findClip(clip.id)->effects[0], unknown);
  ASSERT_EQ(current.sequence().markers.size(), 1U);
  EXPECT_EQ(current.sequence().markers[0], marker);
  ASSERT_EQ(current.sequence().captions.size(), 1U);
  EXPECT_EQ(current.sequence().captions[0], caption);
}

TEST(TimelineEditorTest, AddsAndRemovesTracksAndHonorsTrackLocks) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  Track locked;
  locked.kind = TrackKind::Video;
  locked.name = "Locked";
  locked.locked = true;

  auto added = editor.apply(
      EditCommand{AddTrackCommand{fixture.sequence_id, locked, std::size_t{0}}, {}}, Revision{0});
  ASSERT_TRUE(added);
  const auto current = snapshot(editor, fixture.sequence_id, added.value());
  ASSERT_EQ(current.sequence().tracks.size(), 3U);
  EXPECT_EQ(current.sequence().tracks.front().id, locked.id);

  const auto removal = editor.apply(
      EditCommand{RemoveTrackCommand{fixture.sequence_id, locked.id}, {}}, added.value());
  ASSERT_FALSE(removal);
  EXPECT_EQ(removal.error().code, EditErrorCode::TrackLocked);
}

TEST(TimelineEditorTest, CaptionChangeSetIsAtomicRevisionBoundAndUndoable) {
  auto fixture = makeProject();
  TimelineEditor editor(fixture.project);
  Caption caption;
  caption.range = TimeRange(Time(0, 1), Time(2, 1));
  caption.text = "hello";
  const auto empty = editor.apply(
      EditCommand{ApplyCaptionChangeSetCommand{fixture.sequence_id, {}, {}, {}}, {}}, Revision{0});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.revision(), Revision{0});
  const auto applied = editor.apply(
      EditCommand{ApplyCaptionChangeSetCommand{fixture.sequence_id, {caption}, {}, {}}, {}},
      Revision{0});
  ASSERT_TRUE(applied);
  EXPECT_EQ(editor.history().size(), 1U);
  const auto stale = editor.apply(
      EditCommand{ApplyCaptionChangeSetCommand{fixture.sequence_id, {}, {}, {caption.id}}, {}},
      Revision{0});
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, EditErrorCode::RevisionConflict);
  const auto undone = editor.undo(applied.value());
  ASSERT_TRUE(undone);
  EXPECT_TRUE(snapshot(editor, fixture.sequence_id, undone.value()).sequence().captions.empty());
  const auto redone = editor.redo(undone.value());
  ASSERT_TRUE(redone);
  EXPECT_EQ(snapshot(editor, fixture.sequence_id, redone.value()).sequence().captions.size(), 1U);
}

TEST(TimelineEditorTest, TimelineCutChangeSetRejectsInvalidPayloadsAtomically) {
  auto fixture = makeProject();
  const auto clip = makeClip(fixture.asset_id, 0, 10);
  fixture.project.sequences[0].tracks[0].clips.push_back(clip);
  TimelineEditor editor(fixture.project);
  const auto before = editor.projectAt(Revision{});
  const auto empty = editor.apply(
      EditCommand{ApplyTimelineCutChangeSetCommand{fixture.sequence_id, {}}, {}}, Revision{0});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.projectAt(Revision{}), before);
  auto replacement = TrackClipReplacement{fixture.video_track_id, TrackKind::Video, {clip}};
  replacement.clips.front().timeline_range = TimeRange(Time(1, 1), Time(10, 1));
  const auto missing = editor.apply(
      EditCommand{ApplyTimelineCutChangeSetCommand{
                      fixture.sequence_id, {{EntityId::generate(), TrackKind::Video, {clip}}}},
                  {}},
      Revision{0});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, EditErrorCode::EntityNotFound);
  const auto wrong_kind = editor.apply(
      EditCommand{ApplyTimelineCutChangeSetCommand{
                      fixture.sequence_id, {{fixture.video_track_id, TrackKind::Audio, {clip}}}},
                  {}},
      Revision{0});
  ASSERT_FALSE(wrong_kind);
  EXPECT_EQ(wrong_kind.error().code, EditErrorCode::InvalidTrackKind);
  EXPECT_EQ(editor.revision(), Revision{0});
}

TEST(TimelineEditorTest, RelinkAssetUpdatesReferencedAssetAndOptionalName) {
  auto fixture = makeProject();
  const auto clip = makeClip(fixture.asset_id, 0, 10);
  fixture.project.sequences[0].tracks[0].clips.push_back(clip);
  TimelineEditor editor(fixture.project);

  RelinkAssetCommand relink;
  relink.asset_id = fixture.asset_id;
  relink.source_uri = "/other/newclip.mkv";
  relink.fingerprint = "relinked-fingerprint";
  relink.duration = Time(90, 1);
  relink.has_video = true;
  relink.has_audio = true;
  relink.width = 1920;
  relink.height = 1080;
  relink.nominal_frame_rate = Rate(24, 1);
  relink.audio_sample_rate = 44'100;
  relink.audio_channels = 1;
  relink.metadata.emplace("camera", "B");

  const auto applied = editor.apply(EditCommand{relink, {}}, Revision{0});
  ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);
  EXPECT_EQ(applied.value(), Revision{1});
  EXPECT_EQ(editor.history().front().command_name, "Relink asset");

  const auto current = editor.projectAt(applied.value());
  ASSERT_NE(current, nullptr);
  const auto* asset = findAsset(*current, fixture.asset_id);
  ASSERT_NE(asset, nullptr);
  EXPECT_EQ(asset->id, fixture.asset_id);
  EXPECT_EQ(asset->name, "newclip.mkv");
  EXPECT_EQ(asset->source_uri, "/other/newclip.mkv");
  EXPECT_EQ(asset->fingerprint, "relinked-fingerprint");
  EXPECT_EQ(asset->duration, Time(90, 1));
  EXPECT_TRUE(asset->has_video);
  EXPECT_TRUE(asset->has_audio);
  EXPECT_EQ(asset->width, 1920U);
  EXPECT_EQ(asset->height, 1080U);
  ASSERT_TRUE(asset->nominal_frame_rate.has_value());
  EXPECT_EQ(*asset->nominal_frame_rate, Rate(24, 1));
  EXPECT_EQ(asset->audio_sample_rate, 44'100U);
  EXPECT_EQ(asset->audio_channels, 1U);
  EXPECT_EQ(asset->metadata.at("camera"), "B");

  const auto view = snapshot(editor, fixture.sequence_id, applied.value());
  const auto* live_clip = view.findClip(clip.id);
  ASSERT_NE(live_clip, nullptr);
  EXPECT_EQ(live_clip->asset_id, fixture.asset_id);

  auto customized = makeProject();
  customized.project.assets.front().name = "Hero shot";
  TimelineEditor custom_editor(customized.project);
  RelinkAssetCommand custom_relink = relink;
  custom_relink.asset_id = customized.asset_id;
  const auto custom_applied = custom_editor.apply(EditCommand{custom_relink, {}}, Revision{0});
  ASSERT_TRUE(custom_applied) << (custom_applied ? "" : custom_applied.error().message);
  const auto* custom_asset =
      findAsset(*custom_editor.projectAt(custom_applied.value()), customized.asset_id);
  ASSERT_NE(custom_asset, nullptr);
  EXPECT_EQ(custom_asset->name, "Hero shot");
  EXPECT_EQ(custom_asset->source_uri, "/other/newclip.mkv");

  RelinkAssetCommand missing = relink;
  missing.asset_id = EntityId::generate();
  const auto unknown = editor.apply(EditCommand{missing, {}}, applied.value());
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().code, EditErrorCode::EntityNotFound);
}

} // namespace
} // namespace video_editor::edit
