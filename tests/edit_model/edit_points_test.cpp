// SPDX-License-Identifier: MPL-2.0

#include "video_editor/edit_model/edit_model.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

struct EditPointsFixture final {
  EntityId sequence_id{EntityId::generate()};
  EntityId video_track_id{EntityId::generate()};
  EntityId audio_track_id{EntityId::generate()};
  EntityId asset_id{EntityId::generate()};
  Project project;

  EditPointsFixture() {
    Sequence sequence;
    sequence.id = sequence_id;
    Track video_track;
    video_track.id = video_track_id;
    video_track.kind = TrackKind::Video;
    video_track.targeted = true;
    Clip first;
    first.id = EntityId::generate();
    first.asset_id = asset_id;
    first.timeline_range = TimeRange(Time{}, Time(10, 1));
    first.source_range = TimeRange(Time{}, Time(10, 1));
    Clip second;
    second.id = EntityId::generate();
    second.asset_id = asset_id;
    second.timeline_range = TimeRange(Time(10, 1), Time(8, 1));
    second.source_range = TimeRange(Time(10, 1), Time(8, 1));
    video_track.clips = {first, second};
    Track audio_track;
    audio_track.id = audio_track_id;
    audio_track.kind = TrackKind::Audio;
    audio_track.targeted = false;
    sequence.tracks = {video_track, audio_track};
    project.sequences = {sequence};
  }
};

} // namespace

TEST(EditPointsTest, CollectsClipAndGapBoundaries) {
  EditPointsFixture fixture;
  const auto& sequence = fixture.project.sequences[0];
  const auto points = collectEditPoints(sequence);
  EXPECT_GE(points.size(), 3U);
  EXPECT_EQ(points.front(), Time{});
  EXPECT_TRUE(std::find(points.begin(), points.end(), Time(10, 1)) != points.end());
  EXPECT_TRUE(std::find(points.begin(), points.end(), Time(18, 1)) != points.end());
}

TEST(EditPointsTest, PreviousAndNextEditPoints) {
  EditPointsFixture fixture;
  const auto& sequence = fixture.project.sequences[0];
  EXPECT_EQ(previousEditPoint(sequence, Time(5, 1)), Time{});
  EXPECT_EQ(previousEditPoint(sequence, Time(11, 1)), Time(10, 1));
  EXPECT_EQ(nextEditPoint(sequence, Time(5, 1)), Time(10, 1));
  EXPECT_EQ(nextEditPoint(sequence, Time(10, 1)), Time(18, 1));
  EXPECT_FALSE(nextEditPoint(sequence, Time(18, 1)).has_value());
}

TEST(EditPointsTest, ClipsCoveringPlayheadRespectsTargeting) {
  EditPointsFixture fixture;
  const auto& sequence = fixture.project.sequences[0];
  const auto covering =
      clipsCoveringPlayhead(sequence, CoveringClipQuery{.playhead = Time(5, 1)});
  EXPECT_EQ(covering.size(), 1U);
  EXPECT_EQ(covering.front(), sequence.tracks[0].clips[0].id);

  fixture.project.sequences[0].tracks[0].targeted = false;
  EXPECT_TRUE(
      clipsCoveringPlayhead(fixture.project.sequences[0], CoveringClipQuery{.playhead = Time(5, 1)})
          .empty());
}

TEST(EditPointsTest, SourceTimeAtTimelineTimeHonorsRateAndReverse) {
  Clip clip;
  clip.timeline_range = TimeRange(Time(2, 1), Time(4, 1));
  clip.source_range = TimeRange(Time(20, 1), Time(4, 1));
  clip.playback_rate = Rate(2, 1);
  EXPECT_EQ(sourceTimeAtTimelineTime(clip, Time(3, 1)), Time(22, 1));

  clip.reversed = true;
  EXPECT_EQ(sourceTimeAtTimelineTime(clip, Time(3, 1)), Time(22, 1));
}

TEST(EditPointsTest, SourceTimeClampsToTheSourceRange) {
  Clip clip;
  clip.timeline_range = TimeRange(Time(0, 1), Time(10, 1));
  clip.source_range = TimeRange(Time(5, 1), Time(1, 1));
  EXPECT_EQ(sourceTimeAtTimelineTime(clip, Time(0, 1)), Time(5, 1));
  EXPECT_EQ(sourceTimeAtTimelineTime(clip, Time(9, 1)), Time(5, 1));
}

} // namespace video_editor::edit
