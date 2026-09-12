// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {
namespace edit = video_editor::edit;
namespace captions = video_editor::caption_service;

[[nodiscard]] edit::EntityId makeId(const std::uint8_t seed) {
  std::array<std::uint8_t, 16> bytes{};
  bytes[0] = seed;
  bytes[15] = seed;
  return edit::EntityId(bytes);
}

[[nodiscard]] edit::Time secs(const std::int64_t value) {
  return edit::Time{value, 1};
}

[[nodiscard]] edit::TimeRange span(const std::int64_t start, const std::int64_t duration) {
  return edit::TimeRange{secs(start), secs(duration)};
}

struct Fixture final {
  edit::Project project;
  edit::EntityId sequence_id;
  edit::EntityId asset_id;
  edit::EntityId clip_x; // video, timeline [0,10]
  edit::EntityId clip_y; // video, timeline [10,20]
  edit::EntityId video_track;
  edit::EntityId assembly_track; // empty video track
  edit::EntityId audio_track;
  edit::EntityId audio_clip;
};

[[nodiscard]] Fixture makeFixture() {
  Fixture f;
  edit::Asset asset;
  asset.id = makeId(1);
  asset.name = "A-roll";
  asset.has_video = true;
  asset.has_audio = true;
  asset.width = 1920;
  asset.height = 1080;
  asset.audio_sample_rate = 48'000;
  asset.audio_channels = 2;
  asset.duration = secs(100);
  f.asset_id = asset.id;
  f.project.assets.push_back(asset);

  edit::Sequence sequence;
  sequence.id = makeId(2);
  f.sequence_id = sequence.id;

  edit::Track video;
  video.id = makeId(10);
  video.kind = edit::TrackKind::Video;
  video.name = "Interview";
  f.video_track = video.id;

  edit::Clip clip_x;
  clip_x.id = makeId(20);
  clip_x.asset_id = asset.id;
  clip_x.kind = edit::ClipKind::Video;
  clip_x.name = "Alpha";
  clip_x.timeline_range = span(0, 10);
  clip_x.source_range = span(0, 10);
  f.clip_x = clip_x.id;

  edit::Clip clip_y;
  clip_y.id = makeId(21);
  clip_y.asset_id = asset.id;
  clip_y.kind = edit::ClipKind::Video;
  clip_y.name = "Beta";
  clip_y.timeline_range = span(10, 10);
  clip_y.source_range = span(20, 10);
  f.clip_y = clip_y.id;

  video.clips = {clip_x, clip_y};

  edit::Track assembly;
  assembly.id = makeId(11);
  assembly.kind = edit::TrackKind::Video;
  assembly.name = "Rough cut";
  f.assembly_track = assembly.id;

  edit::Track audio;
  audio.id = makeId(12);
  audio.kind = edit::TrackKind::Audio;
  audio.name = "Sound";
  edit::Clip audio_clip;
  audio_clip.id = makeId(30);
  audio_clip.asset_id = asset.id;
  audio_clip.kind = edit::ClipKind::Audio;
  audio_clip.timeline_range = span(0, 10);
  audio_clip.source_range = span(0, 10);
  audio.clips = {audio_clip};
  f.audio_track = audio.id;
  f.audio_clip = audio_clip.id;

  sequence.tracks = {video, assembly, audio};
  f.project.sequences.push_back(sequence);
  return f;
}

[[nodiscard]] edit::TimelineSnapshot snapshotOf(edit::TimelineEditor& editor,
                                                const edit::EntityId sequence_id) {
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);
  return snapshot.value();
}

TEST(PaperEditTest, AssemblesReorderedPassagesContiguouslyPreservingSourceRanges) {
  const Fixture f = makeFixture();
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);

  // Reverse order: a passage from Beta (12..16), then one from Alpha (2..5).
  const std::vector<captions::PaperEditSelection> selections = {
      {f.clip_y, span(12, 4)},
      {f.clip_x, span(2, 3)},
  };
  const auto result =
      captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  ASSERT_TRUE(result) << (result ? "" : result.error().message);
  const auto& assembly = result.value();

  ASSERT_EQ(assembly.clips.size(), 2U);
  // Passage 0 (from Beta) placed at 0 for 4s; source 20+2 = 22 for 4s.
  EXPECT_EQ(assembly.clips[0].timeline_range, span(0, 4));
  EXPECT_EQ(assembly.clips[0].source_range, span(22, 4));
  EXPECT_EQ(assembly.clips[0].name, "Beta");
  EXPECT_FALSE(assembly.clips[0].linked_group.has_value());
  // Passage 1 (from Alpha) placed contiguously at 4 for 3s; source 0+2 for 3s.
  EXPECT_EQ(assembly.clips[1].timeline_range, span(4, 3));
  EXPECT_EQ(assembly.clips[1].source_range, span(2, 3));
  EXPECT_EQ(assembly.clips[1].name, "Alpha");
  // Distinct deterministic ids that do not collide with the source clips.
  EXPECT_NE(assembly.clips[0].id, assembly.clips[1].id);
  EXPECT_NE(assembly.clips[0].id, f.clip_y);
  EXPECT_EQ(assembly.review_items.size(), 2U);
}

TEST(PaperEditTest, ProducedChangeSetAppliesCleanlyThroughTheEditor) {
  const Fixture f = makeFixture();
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  const std::vector<captions::PaperEditSelection> selections = {
      {f.clip_x, span(0, 5)},
      {f.clip_y, span(15, 5)},
  };
  const auto result =
      captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  ASSERT_TRUE(result) << (result ? "" : result.error().message);

  const auto applied = editor.apply(
      edit::EditCommand{result.value().timeline_change, std::string{}}, editor.revision());
  ASSERT_TRUE(applied) << (applied ? "" : applied.error().message);

  const auto after = editor.snapshot(f.sequence_id, editor.revision());
  ASSERT_TRUE(after);
  const auto* track = after.value().findTrack(f.assembly_track);
  ASSERT_NE(track, nullptr);
  ASSERT_EQ(track->clips.size(), 2U);
  EXPECT_EQ(track->clips[0].timeline_range, span(0, 5));
  EXPECT_EQ(track->clips[1].timeline_range, span(5, 5));
  EXPECT_EQ(track->clips[1].source_range, span(25, 5));
}

TEST(PaperEditTest, HonorsAssemblyStartAndGapBetweenPassages) {
  const Fixture f = makeFixture();
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  const std::vector<captions::PaperEditSelection> selections = {
      {f.clip_x, span(0, 4)},
      {f.clip_x, span(4, 4)},
  };
  captions::PaperEditOptions options;
  options.assembly_start = secs(2);
  options.gap_between = secs(1);
  const auto result =
      captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections, options);
  ASSERT_TRUE(result) << (result ? "" : result.error().message);
  const auto& clips = result.value().clips;
  ASSERT_EQ(clips.size(), 2U);
  EXPECT_EQ(clips[0].timeline_range, span(2, 4)); // starts at assembly_start
  EXPECT_EQ(clips[1].timeline_range, span(7, 4)); // 2+4 + gap(1) = 7
}

TEST(PaperEditTest, MapsReversedSourceClipPassageIntoSourceRangeCorrectly) {
  Fixture f = makeFixture();
  // Make Alpha a reversed clip.
  f.project.sequences.front().tracks.front().clips.front().reversed = true;
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  const std::vector<captions::PaperEditSelection> selections = {{f.clip_x, span(2, 3)}};
  const auto result =
      captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  ASSERT_TRUE(result) << (result ? "" : result.error().message);
  // Reversed: source end (10) - head (2) - dur (3) = 5, duration 3 => [5,3].
  EXPECT_EQ(result.value().clips.front().source_range, span(5, 3));
}

TEST(PaperEditTest, IsDeterministic) {
  const Fixture f = makeFixture();
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  const std::vector<captions::PaperEditSelection> selections = {
      {f.clip_y, span(11, 3)},
      {f.clip_x, span(1, 2)},
  };
  const auto first = captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  const auto second = captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_EQ(first.value().clips.size(), second.value().clips.size());
  for (std::size_t index = 0; index < first.value().clips.size(); ++index) {
    EXPECT_EQ(first.value().clips[index], second.value().clips[index]);
  }
}

TEST(PaperEditTest, RejectsEmptySelections) {
  const Fixture f = makeFixture();
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  const auto result = captions::buildPaperEditAssembly(snapshot, f.assembly_track, {});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, captions::PaperEditErrorCode::NoSelections);
}

TEST(PaperEditTest, RejectsUnknownSourceClip) {
  const Fixture f = makeFixture();
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  const std::vector<captions::PaperEditSelection> selections = {{makeId(99), span(0, 2)}};
  const auto result = captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, captions::PaperEditErrorCode::SourceClipNotFound);
  EXPECT_EQ(result.error().selection_index, 0U);
}

TEST(PaperEditTest, RejectsPassageOutsideSourceClip) {
  const Fixture f = makeFixture();
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  // Alpha spans [0,10]; ask for [8,4] which ends at 12, beyond the clip.
  const std::vector<captions::PaperEditSelection> selections = {{f.clip_x, span(8, 4)}};
  const auto result = captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, captions::PaperEditErrorCode::RangeOutsideClip);
}

TEST(PaperEditTest, RejectsIncompatibleTrackKind) {
  const Fixture f = makeFixture();
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  // Audio passage cannot be assembled onto a video track.
  const std::vector<captions::PaperEditSelection> selections = {{f.audio_clip, span(0, 5)}};
  const auto result = captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, captions::PaperEditErrorCode::IncompatibleTrackKind);
}

TEST(PaperEditTest, RejectsMissingOrLockedTargetTrack) {
  Fixture f = makeFixture();
  f.project.sequences.front().tracks[1].locked = true; // lock the assembly track
  edit::TimelineEditor editor(f.project);
  const auto snapshot = snapshotOf(editor, f.sequence_id);
  const std::vector<captions::PaperEditSelection> selections = {{f.clip_x, span(0, 2)}};

  const auto missing = captions::buildPaperEditAssembly(snapshot, makeId(200), selections);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code, captions::PaperEditErrorCode::TargetTrackNotFound);

  const auto locked = captions::buildPaperEditAssembly(snapshot, f.assembly_track, selections);
  ASSERT_FALSE(locked);
  EXPECT_EQ(locked.error().code, captions::PaperEditErrorCode::TargetTrackLocked);
}

} // namespace
