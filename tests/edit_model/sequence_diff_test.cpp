// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/sequence_diff.h"

#include "video_editor/edit_model/model.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace video_editor::edit {
namespace {

[[nodiscard]] EntityId makeId(const std::uint8_t seed) {
  std::array<std::uint8_t, 16> bytes{};
  bytes[0] = seed;
  bytes[15] = seed;
  return EntityId(bytes);
}

[[nodiscard]] Time seconds(const std::int64_t value) {
  return Time{value, 1};
}

[[nodiscard]] TimeRange span(const std::int64_t start, const std::int64_t duration) {
  return TimeRange{seconds(start), seconds(duration)};
}

[[nodiscard]] Clip makeClip(const std::uint8_t seed, const std::string& name,
                            const std::int64_t start, const std::int64_t duration) {
  Clip clip;
  clip.id = makeId(seed);
  clip.asset_id = makeId(static_cast<std::uint8_t>(200U + seed));
  clip.name = name;
  clip.kind = ClipKind::Video;
  clip.timeline_range = span(start, duration);
  clip.source_range = span(0, duration);
  return clip;
}

[[nodiscard]] Track makeTrack(const std::uint8_t seed, const std::string& name,
                              const TrackKind kind = TrackKind::Video) {
  Track track;
  track.id = makeId(seed);
  track.name = name;
  track.kind = kind;
  return track;
}

[[nodiscard]] const SequenceClipChange* findClipChange(const SequenceDiff& diff,
                                                       const EntityId id) {
  for (const auto& change : diff.clips) {
    if (change.clip_id == id) {
      return &change;
    }
  }
  return nullptr;
}

[[nodiscard]] const FieldChange* findField(const std::vector<FieldChange>& fields,
                                           const std::string& name) {
  for (const auto& field : fields) {
    if (field.field == name) {
      return &field;
    }
  }
  return nullptr;
}

[[nodiscard]] Sequence makeBaseSequence() {
  Sequence sequence;
  sequence.id = makeId(1);
  sequence.name = "Approved cut";
  Track picture = makeTrack(10, "Picture", TrackKind::Video);
  picture.clips = {makeClip(20, "Intro", 0, 10), makeClip(21, "Interview", 10, 30)};
  Track dialogue = makeTrack(11, "Dialogue", TrackKind::Audio);
  dialogue.clips = {makeClip(22, "Boom", 0, 40)};
  sequence.tracks = {picture, dialogue};
  return sequence;
}

TEST(SequenceDiffTest, IdenticalSequencesReportNoChanges) {
  const Sequence sequence = makeBaseSequence();
  const SequenceDiff diff = diffSequences(sequence, sequence);
  EXPECT_TRUE(diff.identical());
  EXPECT_EQ(diff.summary, SequenceDiffSummary{});
  EXPECT_EQ(summarize(diff), "No changes.\n");
}

TEST(SequenceDiffTest, DetectsSequenceLevelPropertyChanges) {
  const Sequence before = makeBaseSequence();
  Sequence after = before;
  after.name = "Client revision 2";
  after.frame_rate = Rate{24, 1};
  after.width = 3840;
  after.height = 2160;

  const SequenceDiff diff = diffSequences(before, after);
  EXPECT_FALSE(diff.identical());
  ASSERT_EQ(diff.sequence_fields.size(), 4U);
  const auto* name = findField(diff.sequence_fields, "name");
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(name->before, "Approved cut");
  EXPECT_EQ(name->after, "Client revision 2");
  EXPECT_NE(findField(diff.sequence_fields, "frame_rate"), nullptr);
  EXPECT_NE(findField(diff.sequence_fields, "width"), nullptr);
  EXPECT_NE(findField(diff.sequence_fields, "height"), nullptr);
}

TEST(SequenceDiffTest, DetectsTrackAddRemoveRenameAndReorder) {
  const Sequence before = makeBaseSequence();
  Sequence after = before;
  // Rename the picture track and add a new music track, remove nothing yet.
  after.tracks[0].name = "Main picture";
  Track music = makeTrack(12, "Music", TrackKind::Audio);
  after.tracks.push_back(music);
  // Reorder: move dialogue before picture.
  std::swap(after.tracks[0], after.tracks[1]);

  const SequenceDiff diff = diffSequences(before, after);
  EXPECT_EQ(diff.summary.tracks_added, 1U);
  EXPECT_EQ(diff.summary.tracks_removed, 0U);
  // Picture is renamed and reordered; dialogue is reordered.
  EXPECT_EQ(diff.summary.tracks_modified, 2U);

  bool saw_added_music = false;
  bool saw_renamed_picture = false;
  for (const auto& change : diff.tracks) {
    if (change.kind == DiffChangeKind::Added && change.label == "Music") {
      saw_added_music = true;
    }
    if (change.kind == DiffChangeKind::Modified && change.track_id == makeId(10)) {
      saw_renamed_picture = true;
      EXPECT_NE(findField(change.fields, "name"), nullptr);
      EXPECT_TRUE(change.before_index.has_value());
      EXPECT_TRUE(change.after_index.has_value());
    }
  }
  EXPECT_TRUE(saw_added_music);
  EXPECT_TRUE(saw_renamed_picture);
}

TEST(SequenceDiffTest, DetectsTrackRemoval) {
  const Sequence before = makeBaseSequence();
  Sequence after = before;
  after.tracks.pop_back(); // remove dialogue track (and its clip)

  const SequenceDiff diff = diffSequences(before, after);
  EXPECT_EQ(diff.summary.tracks_removed, 1U);
  EXPECT_EQ(diff.summary.clips_removed, 1U);
}

TEST(SequenceDiffTest, DetectsClipTrimReportedWithFrameAccurateFields) {
  const Sequence before = makeBaseSequence();
  Sequence after = before;
  // Ripple-trim the head of "Interview": later start, shorter timeline, later
  // source in.
  after.tracks[0].clips[1].timeline_range = span(10, 25);
  after.tracks[0].clips[1].source_range = span(5, 25);

  const SequenceDiff diff = diffSequences(before, after);
  EXPECT_EQ(diff.summary.clips_modified, 1U);
  const auto* change = findClipChange(diff, makeId(21));
  ASSERT_NE(change, nullptr);
  EXPECT_EQ(change->kind, DiffChangeKind::Modified);
  EXPECT_NE(findField(change->fields, "timeline_duration"), nullptr);
  EXPECT_NE(findField(change->fields, "source_start"), nullptr);
  EXPECT_NE(findField(change->fields, "source_duration"), nullptr);
}

TEST(SequenceDiffTest, DetectsClipMoveAcrossTracksAsSingleEdit) {
  const Sequence before = makeBaseSequence();
  Sequence after = before;
  // Move "Intro" clip from Picture (track 10) to a new second video track.
  Track second_picture = makeTrack(13, "Picture 2", TrackKind::Video);
  Clip moved = after.tracks[0].clips[0];
  moved.timeline_range = span(2, 10);
  second_picture.clips = {moved};
  after.tracks[0].clips.erase(after.tracks[0].clips.begin());
  after.tracks.push_back(second_picture);

  const SequenceDiff diff = diffSequences(before, after);
  // The clip is not counted as removed+added; it is one moved modification.
  EXPECT_EQ(diff.summary.clips_removed, 0U);
  EXPECT_EQ(diff.summary.clips_added, 0U);
  EXPECT_EQ(diff.summary.clips_modified, 1U);
  EXPECT_EQ(diff.summary.clips_moved, 1U);
  const auto* change = findClipChange(diff, makeId(20));
  ASSERT_NE(change, nullptr);
  EXPECT_EQ(change->before_track, makeId(10));
  EXPECT_EQ(change->after_track, makeId(13));
  EXPECT_NE(findField(change->fields, "track"), nullptr);
  EXPECT_NE(findField(change->fields, "timeline_start"), nullptr);
}

TEST(SequenceDiffTest, DetectsClipAddRemoveSpeedAndEffectChanges) {
  const Sequence before = makeBaseSequence();
  Sequence after = before;
  // Remove "Boom" audio clip, add a new title clip, retime "Interview".
  after.tracks[1].clips.clear();
  after.tracks[0].clips.push_back(makeClip(30, "Lower third", 40, 5));
  after.tracks[0].clips[1].playback_rate = Rate{1, 2};
  after.tracks[0].clips[1].reversed = true;
  Effect blur;
  blur.id = makeId(60);
  blur.type = "blur";
  after.tracks[0].clips[1].effects.push_back(blur);

  const SequenceDiff diff = diffSequences(before, after);
  EXPECT_EQ(diff.summary.clips_added, 1U);
  EXPECT_EQ(diff.summary.clips_removed, 1U);
  EXPECT_EQ(diff.summary.clips_modified, 1U);
  const auto* change = findClipChange(diff, makeId(21));
  ASSERT_NE(change, nullptr);
  EXPECT_NE(findField(change->fields, "playback_rate"), nullptr);
  EXPECT_NE(findField(change->fields, "reversed"), nullptr);
  EXPECT_NE(findField(change->fields, "effects"), nullptr);
}

TEST(SequenceDiffTest, DetectsMarkerCaptionAndTransitionChanges) {
  Sequence before = makeBaseSequence();
  Marker marker;
  marker.id = makeId(70);
  marker.label = "Act 1";
  marker.range = span(5, 0);
  before.markers = {marker};

  Caption caption;
  caption.id = makeId(71);
  caption.text = "Hello";
  caption.range = span(1, 2);
  before.captions = {caption};

  Sequence after = before;
  after.markers[0].label = "Act one";       // modified
  after.captions[0].text = "Hello world";   // modified
  Transition transition;                     // added
  transition.id = makeId(72);
  transition.kind = TransitionKind::CrossDissolve;
  transition.outgoing_clip_id = makeId(20);
  transition.incoming_clip_id = makeId(21);
  transition.range = span(9, 2);
  after.transitions = {transition};

  const SequenceDiff diff = diffSequences(before, after);
  EXPECT_EQ(diff.summary.markers_changed, 1U);
  EXPECT_EQ(diff.summary.captions_changed, 1U);
  EXPECT_EQ(diff.summary.transitions_changed, 1U);
  ASSERT_EQ(diff.captions.size(), 1U);
  EXPECT_EQ(diff.captions.front().kind, DiffChangeKind::Modified);
  ASSERT_EQ(diff.transitions.size(), 1U);
  EXPECT_EQ(diff.transitions.front().kind, DiffChangeKind::Added);
}

TEST(SequenceDiffTest, SummarizeIsHumanReadableAndDeterministic) {
  const Sequence before = makeBaseSequence();
  Sequence after = before;
  after.name = "Rev 2";
  after.tracks[0].clips[1].timeline_range = span(10, 25);

  const SequenceDiff diff = diffSequences(before, after);
  const std::string first = summarize(diff);
  const std::string second = summarize(diff);
  EXPECT_EQ(first, second);
  EXPECT_NE(first.find("sequence name: Approved cut -> Rev 2"), std::string::npos);
  EXPECT_NE(first.find("clip modified 'Interview'"), std::string::npos);
}

} // namespace
} // namespace video_editor::edit
