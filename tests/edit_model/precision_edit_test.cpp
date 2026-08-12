// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <utility>

namespace video_editor::edit {
namespace {

struct PrecisionProject final {
  Project project;
  EntityId asset_id;
  EntityId sequence_id;
  EntityId video_track_id;
  EntityId second_video_track_id;
  EntityId audio_track_id;
};

[[nodiscard]] PrecisionProject makePrecisionProject() {
  PrecisionProject result;
  result.project.name = "Precision edit test";

  Asset asset;
  asset.name = "source.mov";
  asset.source_uri = "/media/source.mov";
  asset.duration = Time(240, 1);
  asset.has_video = true;
  asset.has_audio = true;
  asset.width = 1920;
  asset.height = 1080;
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

  Track video_two;
  video_two.kind = TrackKind::Video;
  video_two.name = "V2";
  result.second_video_track_id = video_two.id;
  sequence.tracks.push_back(video_two);

  Track audio;
  audio.kind = TrackKind::Audio;
  audio.name = "A1";
  result.audio_track_id = audio.id;
  sequence.tracks.push_back(audio);

  result.project.sequences.push_back(sequence);
  return result;
}

[[nodiscard]] Clip precisionClip(EntityId asset_id, ClipKind kind, std::int64_t timeline_start,
                                 std::int64_t duration, std::int64_t source_start) {
  Clip clip;
  clip.asset_id = asset_id;
  clip.kind = kind;
  clip.timeline_range = TimeRange(Time(timeline_start, 1), Time(duration, 1));
  clip.source_range = TimeRange(Time(source_start, 1), Time(duration, 1));
  return clip;
}

[[nodiscard]] Revision applyOrCurrent(TimelineEditor& editor, Revision revision,
                                      EditCommand command) {
  auto result = editor.apply(std::move(command), revision);
  EXPECT_TRUE(result) << (result ? "" : result.error().message);
  return result ? result.value() : revision;
}

[[nodiscard]] TimelineSnapshot currentSnapshot(TimelineEditor& editor, EntityId sequence_id,
                                               Revision revision) {
  auto result = editor.snapshot(sequence_id, revision);
  EXPECT_TRUE(result) << (result ? "" : result.error().message);
  return result ? result.value() : TimelineSnapshot{};
}

TEST(PrecisionEditTest, RollMovesOnlyTheCutAndMapsSourceAtClipRates) {
  auto fixture = makePrecisionProject();
  auto left = precisionClip(fixture.asset_id, ClipKind::Video, 0, 5, 10);
  auto right = precisionClip(fixture.asset_id, ClipKind::Video, 5, 5, 30);
  left.playback_rate = Rate(2, 1);
  right.playback_rate = Rate(2, 1);
  left.source_range.duration = Time(10, 1);
  right.source_range.duration = Time(10, 1);
  fixture.project.sequences[0].tracks[0].clips = {left, right};
  TimelineEditor editor(fixture.project);

  auto applied = editor.apply(
      EditCommand{RollEditCommand{fixture.sequence_id, left.id, right.id, Time(6, 1)}, "roll-1"},
      Revision{0});
  ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);
  const auto current = currentSnapshot(editor, fixture.sequence_id, applied.value());
  const auto* rolled_left = current.findClip(left.id);
  const auto* rolled_right = current.findClip(right.id);
  ASSERT_NE(rolled_left, nullptr);
  ASSERT_NE(rolled_right, nullptr);
  EXPECT_EQ(rolled_left->timeline_range, TimeRange(Time{}, Time(6, 1)));
  EXPECT_EQ(rolled_right->timeline_range, TimeRange(Time(6, 1), Time(4, 1)));
  EXPECT_EQ(rolled_left->source_range, TimeRange(Time(10, 1), Time(12, 1)));
  EXPECT_EQ(rolled_right->source_range, TimeRange(Time(32, 1), Time(8, 1)));
  EXPECT_EQ(current.duration(), Time(10, 1));

  const auto undone = editor.undo(applied.value());
  ASSERT_TRUE(undone);
  EXPECT_EQ(*editor.projectAt(undone.value()), fixture.project);
  const auto redone = editor.redo(undone.value());
  ASSERT_TRUE(redone);
  EXPECT_EQ(currentSnapshot(editor, fixture.sequence_id, redone.value())
                .findClip(left.id)
                ->timeline_range.duration,
            Time(6, 1));
}

TEST(PrecisionEditTest, RollUnderstandsReversedSourceOrientation) {
  auto fixture = makePrecisionProject();
  auto left = precisionClip(fixture.asset_id, ClipKind::Video, 0, 5, 20);
  auto right = precisionClip(fixture.asset_id, ClipKind::Video, 5, 5, 40);
  left.reversed = true;
  right.reversed = true;
  fixture.project.sequences[0].tracks[0].clips = {left, right};
  TimelineEditor editor(fixture.project);

  const auto revision = applyOrCurrent(
      editor, Revision{0},
      EditCommand{RollEditCommand{fixture.sequence_id, left.id, right.id, Time(6, 1)}, {}});
  const auto current = currentSnapshot(editor, fixture.sequence_id, revision);
  EXPECT_EQ(current.findClip(left.id)->source_range, TimeRange(Time(19, 1), Time(6, 1)));
  EXPECT_EQ(current.findClip(right.id)->source_range, TimeRange(Time(40, 1), Time(4, 1)));
}

TEST(PrecisionEditTest, RollRejectsGapsAndInsufficientHandlesAtomically) {
  auto fixture = makePrecisionProject();
  auto left = precisionClip(fixture.asset_id, ClipKind::Video, 0, 5, 0);
  auto right = precisionClip(fixture.asset_id, ClipKind::Video, 6, 5, 0);
  fixture.project.sequences[0].tracks[0].clips = {left, right};
  TimelineEditor editor(fixture.project);

  auto rejected = editor.apply(
      EditCommand{RollEditCommand{fixture.sequence_id, left.id, right.id, Time(4, 1)}, {}},
      Revision{0});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.revision(), Revision{0});

  fixture.project.sequences[0].tracks[0].clips[1].timeline_range.start = Time(5, 1);
  TimelineEditor handle_editor(fixture.project);
  rejected = handle_editor.apply(
      EditCommand{RollEditCommand{fixture.sequence_id, left.id, right.id, Time(4, 1)}, {}},
      Revision{0});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(handle_editor.revision(), Revision{0});
}

TEST(PrecisionEditTest, SlipPreservesTimelineAndMovesLinkedSourceWindows) {
  auto fixture = makePrecisionProject();
  const auto group = EntityId::generate();
  auto video = precisionClip(fixture.asset_id, ClipKind::Video, 10, 8, 20);
  auto audio = precisionClip(fixture.asset_id, ClipKind::Audio, 10, 8, 40);
  video.linked_group = group;
  audio.linked_group = group;
  fixture.project.sequences[0].tracks[0].clips.push_back(video);
  fixture.project.sequences[0].tracks[2].clips.push_back(audio);
  TimelineEditor editor(fixture.project);

  const auto revision = applyOrCurrent(
      editor, Revision{0},
      EditCommand{SlipClipCommand{fixture.sequence_id, video.id, Time(25, 1), true}, {}});
  const auto current = currentSnapshot(editor, fixture.sequence_id, revision);
  EXPECT_EQ(current.findClip(video.id)->timeline_range, video.timeline_range);
  EXPECT_EQ(current.findClip(audio.id)->timeline_range, audio.timeline_range);
  EXPECT_EQ(current.findClip(video.id)->source_range, TimeRange(Time(25, 1), Time(8, 1)));
  EXPECT_EQ(current.findClip(audio.id)->source_range, TimeRange(Time(45, 1), Time(8, 1)));
}

TEST(PrecisionEditTest, SlipRejectsOutOfBoundsAndLockedLinkedCompanion) {
  auto fixture = makePrecisionProject();
  const auto group = EntityId::generate();
  auto video = precisionClip(fixture.asset_id, ClipKind::Video, 0, 8, 0);
  auto audio = precisionClip(fixture.asset_id, ClipKind::Audio, 0, 8, 0);
  video.linked_group = group;
  audio.linked_group = group;
  fixture.project.sequences[0].tracks[0].clips.push_back(video);
  fixture.project.sequences[0].tracks[2].clips.push_back(audio);
  fixture.project.sequences[0].tracks[2].locked = true;
  TimelineEditor editor(fixture.project);

  auto rejected = editor.apply(
      EditCommand{SlipClipCommand{fixture.sequence_id, video.id, Time(1, 1), true}, {}},
      Revision{0});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, EditErrorCode::TrackLocked);
  EXPECT_EQ(editor.revision(), Revision{0});

  rejected = editor.apply(
      EditCommand{SlipClipCommand{fixture.sequence_id, video.id, Time(235, 1), false}, {}},
      Revision{0});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, EditErrorCode::InvalidArgument);
  EXPECT_EQ(editor.revision(), Revision{0});
}

TEST(PrecisionEditTest, SlideTrimsNeighborsAndKeepsSelectedSourceWindow) {
  auto fixture = makePrecisionProject();
  const auto previous = precisionClip(fixture.asset_id, ClipKind::Video, 0, 5, 10);
  const auto selected = precisionClip(fixture.asset_id, ClipKind::Video, 5, 5, 30);
  const auto next = precisionClip(fixture.asset_id, ClipKind::Video, 10, 5, 50);
  fixture.project.sequences[0].tracks[0].clips = {previous, selected, next};
  TimelineEditor editor(fixture.project);

  const auto revision = applyOrCurrent(
      editor, Revision{0},
      EditCommand{SlideClipCommand{fixture.sequence_id, selected.id, Time(7, 1)}, {}});
  const auto current = currentSnapshot(editor, fixture.sequence_id, revision);
  EXPECT_EQ(current.findClip(previous.id)->timeline_range, TimeRange(Time{}, Time(7, 1)));
  EXPECT_EQ(current.findClip(previous.id)->source_range, TimeRange(Time(10, 1), Time(7, 1)));
  EXPECT_EQ(current.findClip(selected.id)->timeline_range, TimeRange(Time(7, 1), Time(5, 1)));
  EXPECT_EQ(current.findClip(selected.id)->source_range, selected.source_range);
  EXPECT_EQ(current.findClip(next.id)->timeline_range, TimeRange(Time(12, 1), Time(3, 1)));
  EXPECT_EQ(current.findClip(next.id)->source_range, TimeRange(Time(52, 1), Time(3, 1)));
  EXPECT_EQ(current.duration(), Time(15, 1));
}

TEST(PrecisionEditTest, LinkedMoveUsesExactDeltaAndDefaultsToSingleClip) {
  auto fixture = makePrecisionProject();
  const auto group = EntityId::generate();
  auto video = precisionClip(fixture.asset_id, ClipKind::Video, 5, 5, 20);
  auto audio = precisionClip(fixture.asset_id, ClipKind::Audio, 5, 5, 20);
  video.linked_group = group;
  audio.linked_group = group;
  fixture.project.sequences[0].tracks[0].clips.push_back(video);
  fixture.project.sequences[0].tracks[2].clips.push_back(audio);

  TimelineEditor single_editor(fixture.project);
  auto revision = applyOrCurrent(
      single_editor, Revision{0},
      EditCommand{MoveClipCommand{fixture.sequence_id, video.id, fixture.video_track_id, Time(9, 1),
                                  InsertMode::RejectOverlap},
                  {}});
  auto current = currentSnapshot(single_editor, fixture.sequence_id, revision);
  EXPECT_EQ(current.findClip(video.id)->timeline_range.start, Time(9, 1));
  EXPECT_EQ(current.findClip(audio.id)->timeline_range.start, Time(5, 1));

  TimelineEditor linked_editor(fixture.project);
  revision = applyOrCurrent(
      linked_editor, Revision{0},
      EditCommand{MoveClipCommand{fixture.sequence_id, video.id, fixture.second_video_track_id,
                                  Time(9, 1), InsertMode::RejectOverlap, true},
                  {}});
  current = currentSnapshot(linked_editor, fixture.sequence_id, revision);
  EXPECT_EQ(current.findClip(video.id)->timeline_range.start, Time(9, 1));
  EXPECT_EQ(current.findClip(audio.id)->timeline_range.start, Time(9, 1));
  EXPECT_EQ(current.findTrack(fixture.video_track_id)->clips.size(), 0U);
  ASSERT_EQ(current.findTrack(fixture.second_video_track_id)->clips.size(), 1U);
  EXPECT_EQ(current.findTrack(fixture.second_video_track_id)->clips[0].id, video.id);
  ASSERT_EQ(current.findTrack(fixture.audio_track_id)->clips.size(), 1U);
}

TEST(PrecisionEditTest, LinkedTrimMapsForwardAndReverseCompanionSources) {
  auto fixture = makePrecisionProject();
  const auto group = EntityId::generate();
  auto video = precisionClip(fixture.asset_id, ClipKind::Video, 10, 10, 20);
  auto audio = precisionClip(fixture.asset_id, ClipKind::Audio, 10, 10, 40);
  video.linked_group = group;
  audio.linked_group = group;
  audio.reversed = true;
  fixture.project.sequences[0].tracks[0].clips.push_back(video);
  fixture.project.sequences[0].tracks[2].clips.push_back(audio);
  TimelineEditor editor(fixture.project);

  const auto revision = applyOrCurrent(
      editor, Revision{0},
      EditCommand{TrimClipCommand{fixture.sequence_id, video.id, TimeRange(Time(12, 1), Time(7, 1)),
                                  TimeRange(Time(22, 1), Time(7, 1)), true},
                  {}});
  const auto current = currentSnapshot(editor, fixture.sequence_id, revision);
  EXPECT_EQ(current.findClip(video.id)->timeline_range, TimeRange(Time(12, 1), Time(7, 1)));
  EXPECT_EQ(current.findClip(audio.id)->timeline_range, TimeRange(Time(12, 1), Time(7, 1)));
  // Head +2 removes from the high end; tail -1 removes from the low end.
  EXPECT_EQ(current.findClip(audio.id)->source_range, TimeRange(Time(41, 1), Time(7, 1)));
}

TEST(PrecisionEditTest, LinkedRippleRemovalClosesEachAffectedTrack) {
  auto fixture = makePrecisionProject();
  const auto group = EntityId::generate();
  auto video = precisionClip(fixture.asset_id, ClipKind::Video, 0, 4, 20);
  auto audio = precisionClip(fixture.asset_id, ClipKind::Audio, 0, 4, 20);
  video.linked_group = group;
  audio.linked_group = group;
  const auto later_video = precisionClip(fixture.asset_id, ClipKind::Video, 4, 3, 60);
  const auto later_audio = precisionClip(fixture.asset_id, ClipKind::Audio, 4, 3, 60);
  fixture.project.sequences[0].tracks[0].clips = {video, later_video};
  fixture.project.sequences[0].tracks[2].clips = {audio, later_audio};
  TimelineEditor editor(fixture.project);

  const auto revision =
      applyOrCurrent(editor, Revision{0},
                     EditCommand{RemoveClipCommand{fixture.sequence_id, video.id, true, true}, {}});
  const auto current = currentSnapshot(editor, fixture.sequence_id, revision);
  EXPECT_EQ(current.findClip(video.id), nullptr);
  EXPECT_EQ(current.findClip(audio.id), nullptr);
  EXPECT_EQ(current.findClip(later_video.id)->timeline_range.start, Time{});
  EXPECT_EQ(current.findClip(later_audio.id)->timeline_range.start, Time{});
}

TEST(SnappingTest, SortsExactTiesByStableProfessionalPriority) {
  auto fixture = makePrecisionProject();
  auto& sequence = fixture.project.sequences[0];
  sequence.frame_rate = Rate(1, 1);
  auto clip = precisionClip(fixture.asset_id, ClipKind::Video, 10, 5, 20);
  sequence.tracks[0].clips.push_back(clip);
  Marker marker;
  marker.range = TimeRange(Time(10, 1), Time{});
  sequence.markers.push_back(marker);

  const SnapRequest request{Time(10, 1), Time{}, Time(10, 1), true, true, true};
  const auto first = findSnapCandidates(sequence, request);
  const auto second = findSnapCandidates(sequence, request);
  EXPECT_EQ(first, second);
  ASSERT_EQ(first.size(), 4U);
  EXPECT_EQ(first[0].kind, SnapTargetKind::Playhead);
  EXPECT_EQ(first[1].kind, SnapTargetKind::Marker);
  EXPECT_EQ(first[2].kind, SnapTargetKind::ClipEdge);
  EXPECT_EQ(first[3].kind, SnapTargetKind::FrameGrid);
  ASSERT_TRUE(nearestSnapCandidate(sequence, request));
  EXPECT_EQ(nearestSnapCandidate(sequence, request)->kind, SnapTargetKind::Playhead);
}

TEST(SnappingTest, UsesExactNtscFrameGridAndInclusiveThreshold) {
  auto fixture = makePrecisionProject();
  auto& sequence = fixture.project.sequences[0];
  sequence.frame_rate = Rate(30'000, 1'001);
  const SnapRequest request{Time(1, 30), Time(1, 30'000), std::nullopt, false, false, true};
  const auto candidates = findSnapCandidates(sequence, request);
  ASSERT_EQ(candidates.size(), 1U);
  EXPECT_EQ(candidates[0].time, Time(1'001, 30'000));
  EXPECT_EQ(candidates[0].distance, Time(1, 30'000));
  EXPECT_EQ(candidates[0].frame_number, 1);

  const SnapRequest too_small{Time(1, 30), Time(1, 30'001), std::nullopt, false, false, true};
  EXPECT_TRUE(findSnapCandidates(sequence, too_small).empty());
  EXPECT_THROW(
      {
        const auto ignored = findSnapCandidates(
            sequence, SnapRequest{Time{}, Time(-1, 1), std::nullopt, false, false, false});
        (void)ignored;
      },
      std::invalid_argument);
}

TEST(SnappingTest, ExcludesMovingClipEdgesWithoutChangingNtscGrid) {
  Sequence sequence;
  sequence.frame_rate = Rate(30'000, 1'001);
  Track track;
  track.kind = TrackKind::Video;
  track.name = "V1";
  Clip clip;
  clip.timeline_range = TimeRange(Time(1, 1), Time(1, 1));
  track.clips.push_back(clip);
  sequence.tracks.push_back(track);
  SnapRequest request{Time(1, 1), Time(1, 1), std::nullopt, true, false, true};
  request.excluded_clip_ids.insert(clip.id);
  const auto candidates = findSnapCandidates(sequence, request);
  EXPECT_TRUE(
      std::none_of(candidates.begin(), candidates.end(),
                   [&](const SnapCandidate& candidate) { return candidate.entity_id == clip.id; }));
  ASSERT_TRUE(nearestSnapCandidate(sequence, request));
  EXPECT_EQ(nearestSnapCandidate(sequence, request)->kind, SnapTargetKind::FrameGrid);
}

TEST(SnappingTest, ExcludesBothEdgesOfTheMarkerBeingDragged) {
  Sequence sequence;
  sequence.frame_rate = Rate(1, 1);
  Marker dragged;
  dragged.range = TimeRange(Time(10, 1), Time(2, 1));
  Marker other;
  other.range = TimeRange(Time(11, 1), Time{});
  sequence.markers = {dragged, other};

  SnapRequest request{Time(10, 1), Time(2, 1), std::nullopt, false, true, false};
  request.excluded_marker_ids.insert(dragged.id);
  const auto candidates = findSnapCandidates(sequence, request);

  ASSERT_EQ(candidates.size(), 1U);
  EXPECT_EQ(candidates.front().kind, SnapTargetKind::Marker);
  EXPECT_EQ(candidates.front().entity_id, other.id);
  EXPECT_EQ(candidates.front().time, Time(11, 1));
}

void expectTrackInvariants(const Project& project, const Track& track) {
  const Clip* previous = nullptr;
  for (const auto& clip : track.clips) {
    EXPECT_FALSE(clip.timeline_range.start.isNegative());
    EXPECT_GT(clip.timeline_range.duration, Time{});
    EXPECT_FALSE(clip.source_range.start.isNegative());
    EXPECT_GT(clip.source_range.duration, Time{});
    const auto* asset = findAsset(project, clip.asset_id);
    ASSERT_NE(asset, nullptr);
    EXPECT_LE(clip.source_range.end(), asset->duration);
    if (previous != nullptr) {
      EXPECT_LE(previous->timeline_range.end(), clip.timeline_range.start);
    }
    previous = &clip;
  }
}

TEST(PrecisionEditProperties, RandomizedOperationsPreserveInvariantsAndUndoRedoExactly) {
  std::mt19937 random(0x5EED'1234U);
  std::uniform_int_distribution<std::int64_t> duration_distribution(3, 12);

  for (int iteration = 0; iteration < 180; ++iteration) {
    auto fixture = makePrecisionProject();
    const auto first_duration = duration_distribution(random);
    const auto middle_duration = duration_distribution(random);
    const auto final_duration = duration_distribution(random);
    auto first = precisionClip(fixture.asset_id, ClipKind::Video, 0, first_duration, 30);
    auto middle =
        precisionClip(fixture.asset_id, ClipKind::Video, first_duration, middle_duration, 90);
    auto final = precisionClip(fixture.asset_id, ClipKind::Video, first_duration + middle_duration,
                               final_duration, 150);
    if ((iteration % 2) != 0) {
      first.reversed = true;
      final.reversed = true;
    }
    fixture.project.sequences[0].tracks[0].clips = {first, middle, final};
    TimelineEditor editor(fixture.project);
    const auto before = *editor.projectAt(Revision{0});

    EditCommand command;
    switch (iteration % 3) {
    case 0: {
      const auto minimum_delta = -(first_duration - 1);
      const auto maximum_delta = middle_duration - 1;
      std::uniform_int_distribution<std::int64_t> delta_distribution(minimum_delta, maximum_delta);
      const auto delta = delta_distribution(random);
      command = EditCommand{RollEditCommand{fixture.sequence_id, first.id, middle.id,
                                            Time(first_duration + delta, 1)},
                            {}};
      break;
    }
    case 1: {
      const auto minimum_delta = -(first_duration - 1);
      const auto maximum_delta = final_duration - 1;
      std::uniform_int_distribution<std::int64_t> delta_distribution(minimum_delta, maximum_delta);
      const auto delta = delta_distribution(random);
      command = EditCommand{
          SlideClipCommand{fixture.sequence_id, middle.id, Time(first_duration + delta, 1)}, {}};
      break;
    }
    default: {
      std::uniform_int_distribution<std::int64_t> source_distribution(0, 240 - middle_duration);
      command = EditCommand{SlipClipCommand{fixture.sequence_id, middle.id,
                                            Time(source_distribution(random), 1), false},
                            {}};
      break;
    }
    }

    const auto applied = editor.apply(std::move(command), Revision{0});
    ASSERT_TRUE(applied) << "iteration " << iteration << ": "
                         << (applied ? "" : applied.error().message);
    const auto after = *editor.projectAt(applied.value());
    const auto* sequence = findSequence(after, fixture.sequence_id);
    ASSERT_NE(sequence, nullptr);
    for (const auto& track : sequence->tracks) {
      expectTrackInvariants(after, track);
    }

    const auto undone = editor.undo(applied.value());
    ASSERT_TRUE(undone) << "iteration " << iteration;
    EXPECT_EQ(*editor.projectAt(undone.value()), before) << "iteration " << iteration;
    const auto redone = editor.redo(undone.value());
    ASSERT_TRUE(redone) << "iteration " << iteration;
    EXPECT_EQ(*editor.projectAt(redone.value()), after) << "iteration " << iteration;
  }
}

} // namespace
} // namespace video_editor::edit
