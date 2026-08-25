// SPDX-License-Identifier: MPL-2.0
#include "video_editor/caption_service/caption_service.h"

#include <gtest/gtest.h>

#include <array>

namespace {
namespace edit = video_editor::edit;
namespace captions = video_editor::caption_service;

edit::Project project_with_tracks() {
  edit::Project project;
  edit::Asset asset;
  asset.has_audio = true;
  asset.has_video = true;
  asset.width = 1920;
  asset.height = 1080;
  asset.audio_sample_rate = 48'000;
  asset.audio_channels = 2;
  asset.duration = edit::Time(20, 1);
  project.assets.push_back(asset);
  edit::Sequence sequence;
  edit::Track track;
  track.kind = edit::TrackKind::Audio;
  edit::Clip clip;
  clip.kind = edit::ClipKind::Audio;
  clip.asset_id = asset.id;
  clip.timeline_range = edit::TimeRange(edit::Time(0, 1), edit::Time(10, 1));
  clip.source_range = clip.timeline_range;
  track.clips.push_back(clip);
  auto later = clip;
  later.id = edit::EntityId::generate();
  later.timeline_range = edit::TimeRange(edit::Time(12, 1), edit::Time(6, 1));
  later.source_range = edit::TimeRange(edit::Time(12, 1), edit::Time(6, 1));
  track.clips.push_back(later);
  sequence.tracks.push_back(track);
  project.sequences.push_back(sequence);
  return project;
}

edit::Project project_with_linked_av_tracks(const bool locked_audio = false) {
  auto project = project_with_tracks();
  auto& sequence = project.sequences.front();
  auto& audio = sequence.tracks.front();
  audio.locked = locked_audio;
  const auto group = edit::EntityId::generate();
  audio.clips.front().linked_group = group;
  audio.clips.back().linked_group = group;
  auto video = audio.clips.front();
  video.id = edit::EntityId::generate();
  video.kind = edit::ClipKind::Video;
  video.linked_group = group;
  edit::Track video_track;
  video_track.kind = edit::TrackKind::Video;
  video_track.clips.push_back(video);
  auto video_later = audio.clips.back();
  video_later.id = edit::EntityId::generate();
  video_later.kind = edit::ClipKind::Video;
  video_later.linked_group = group;
  video_track.clips.push_back(video_later);
  sequence.tracks.push_back(video_track);
  return project;
}

TEST(TranscriptConversion, KeepsWordTimingProbabilityAndProvenance) {
  captions::CaptionCue cue;
  cue.range = edit::TimeRange(edit::Time(0, 1), edit::Time(2, 1));
  cue.text = "hello world";
  cue.provenance = {.source = edit::CaptionWordSource::LocalTranscription,
                    .model_identity = "whisper-base-sha1"};
  cue.words = {{.text = "hello",
                .range = edit::TimeRange(edit::Time(0, 1), edit::Time(1, 2)),
                .probability = 0.8},
               {.text = "world",
                .range = edit::TimeRange(edit::Time(1, 1), edit::Time(1, 2)),
                .probability = 0.9}};
  const auto caption = captions::toEditCaption(cue, "en");
  EXPECT_EQ(caption.words, cue.words);
  EXPECT_EQ(caption.provenance, cue.provenance);
  EXPECT_EQ(captions::fromEditCaption(caption).words, cue.words);
}

TEST(TranscriptConversion, SearchHitCarriesWordIdentityAndExactTime) {
  captions::CaptionCue cue;
  cue.range = edit::TimeRange(edit::Time(0, 1), edit::Time(2, 1));
  cue.text = "hello world";
  cue.words = {{.text = "hello", .range = edit::TimeRange(edit::Time(1, 2), edit::Time(1, 2))},
               {.text = "world", .range = edit::TimeRange(edit::Time(1, 1), edit::Time(1, 2))}};
  const auto result = captions::search(std::span<const captions::CaptionCue>(&cue, 1), "world");
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 1U);
  ASSERT_TRUE(result.value().front().word_id.has_value());
  EXPECT_EQ(*result.value().front().word_id, cue.words[1].id);
  EXPECT_EQ(result.value().front().word_range, cue.words[1].range);
}

TEST(TranscriptProposal, ProducesAtomicRippleReplacementAndPreservesUncutIds) {
  edit::TimelineEditor editor(project_with_tracks());
  const auto sequence_id = editor.projectAt(edit::Revision{})->sequences.front().id;
  const auto snapshot = editor.snapshot(sequence_id, editor.revision());
  ASSERT_TRUE(snapshot);
  const auto selected = edit::TimeRange(edit::Time(2, 1), edit::Time(1, 1));
  const auto proposal = captions::buildTimelineCutProposal(snapshot.value(), {&selected, 1});
  ASSERT_TRUE(proposal);
  ASSERT_TRUE(proposal.value().timeline_cuts.has_value());
  ASSERT_EQ(proposal.value().timeline_cuts->tracks.size(), 1U);
  const auto& replacement = proposal.value().timeline_cuts->tracks.front().clips;
  ASSERT_EQ(replacement.size(), 3U);
  EXPECT_NE(replacement[0].id, replacement[1].id);
  EXPECT_EQ(replacement[2].id, snapshot.value().sequence().tracks.front().clips[1].id);

  const auto result = editor.apply(edit::EditCommand{*proposal.value().timeline_cuts, {}},
                                   proposal.value().base_revision);
  ASSERT_TRUE(result);
  EXPECT_EQ(editor.history().size(), 1U);
  const auto undone = editor.undo(result.value());
  ASSERT_TRUE(undone);
  EXPECT_EQ(editor.projectAt(undone.value())->sequences.front().tracks.front().clips.size(), 2U);
}

TEST(TranscriptProposal, RejectsUnsortedSelectedRanges) {
  edit::TimelineEditor editor(project_with_tracks());
  const auto sequence_id = editor.projectAt(edit::Revision{})->sequences.front().id;
  const auto snapshot = editor.snapshot(sequence_id, editor.revision());
  ASSERT_TRUE(snapshot);
  const std::array<edit::TimeRange, 2> selected{
      edit::TimeRange(edit::Time(5, 1), edit::Time(1, 1)),
      edit::TimeRange(edit::Time(2, 1), edit::Time(1, 1))};
  const auto proposal = captions::buildTimelineCutProposal(snapshot.value(), selected);
  ASSERT_FALSE(proposal);
  EXPECT_EQ(proposal.error().code, captions::ProposalErrorCode::Overlap);
}

TEST(TranscriptProposal, RippleMapsCaptionAndWordTimingThroughCut) {
  auto project = project_with_tracks();
  auto& caption = project.sequences.front().captions.emplace_back();
  caption.range = edit::TimeRange(edit::Time(1, 1), edit::Time(8, 1));
  caption.text = "one two three";
  caption.words = {{.text = "one", .range = edit::TimeRange(edit::Time(1, 1), edit::Time(2, 1))},
                   {.text = "two", .range = edit::TimeRange(edit::Time(4, 1), edit::Time(2, 1))},
                   {.text = "three", .range = edit::TimeRange(edit::Time(7, 1), edit::Time(2, 1))}};
  edit::TimelineEditor editor(std::move(project));
  const auto snapshot =
      editor.snapshot(editor.projectAt(edit::Revision{})->sequences.front().id, editor.revision());
  ASSERT_TRUE(snapshot);
  const edit::TimeRange cut(edit::Time(5, 1), edit::Time(1, 1));
  const auto proposal = captions::buildTimelineCutProposal(snapshot.value(), {&cut, 1});
  ASSERT_TRUE(proposal);
  ASSERT_EQ(proposal.value().caption_changes.updated.size(), 1U);
  const auto& mapped = proposal.value().caption_changes.updated.front();
  EXPECT_EQ(mapped.text, "one two three");
  EXPECT_EQ(mapped.words[2].range.start, edit::Time(6, 1));
  EXPECT_EQ(mapped.words[2].range.duration, edit::Time(2, 1));
}

TEST(TranscriptProposal, ShiftsTransitionWhenCutIsBeforeTransitionRange) {
  auto project = project_with_tracks();
  auto& sequence = project.sequences.front();
  auto& track = sequence.tracks.front();
  track.kind = edit::TrackKind::Video;
  track.clips.front().kind = edit::ClipKind::Video;
  track.clips.back().kind = edit::ClipKind::Video;
  track.clips.back().timeline_range = edit::TimeRange(edit::Time(10, 1), edit::Time(6, 1));
  track.clips.back().source_range = edit::TimeRange(edit::Time(10, 1), edit::Time(6, 1));
  edit::Transition transition;
  transition.outgoing_clip_id = track.clips.front().id;
  transition.incoming_clip_id = track.clips.back().id;
  transition.range = edit::TimeRange(edit::Time(9, 1), edit::Time(2, 1));
  sequence.transitions.push_back(transition);
  edit::TimelineEditor editor(std::move(project));
  const auto snapshot = editor.snapshot(sequence.id, editor.revision());
  ASSERT_TRUE(snapshot);
  const edit::TimeRange cut(edit::Time(0, 1), edit::Time(1, 1));
  const auto proposal = captions::buildTimelineCutProposal(snapshot.value(), {&cut, 1});
  ASSERT_TRUE(proposal);
  ASSERT_TRUE(proposal.value().timeline_cuts.has_value());
  ASSERT_TRUE(proposal.value().timeline_cuts->transitions.has_value());
  ASSERT_EQ(proposal.value().timeline_cuts->transitions->size(), 1U);
  const auto& remapped = proposal.value().timeline_cuts->transitions->front();
  EXPECT_EQ(remapped.id, transition.id);
  EXPECT_EQ(remapped.range, edit::TimeRange(edit::Time(8, 1), edit::Time(2, 1)));

  const auto result = editor.apply(edit::EditCommand{*proposal.value().timeline_cuts, {}},
                                   proposal.value().base_revision);
  ASSERT_TRUE(result);
  const auto after =
      editor.snapshot(sequence.id, result.value());
  ASSERT_TRUE(after);
  ASSERT_EQ(after.value().sequence().transitions.size(), 1U);
  EXPECT_EQ(after.value().sequence().transitions.front().range,
            edit::TimeRange(edit::Time(8, 1), edit::Time(2, 1)));
}

TEST(TranscriptProposal, RippleKeepsLinkedAvSegmentsAndSynchronizesTrailingMaterial) {
  edit::TimelineEditor editor(project_with_linked_av_tracks());
  const auto sequence_id = editor.projectAt(edit::Revision{})->sequences.front().id;
  const auto snapshot = editor.snapshot(sequence_id, editor.revision());
  ASSERT_TRUE(snapshot);
  const std::array<edit::TimeRange, 2> selected{
      edit::TimeRange(edit::Time(2, 1), edit::Time(1, 1)),
      edit::TimeRange(edit::Time(5, 1), edit::Time(1, 1))};
  const auto first = captions::buildTimelineCutProposal(snapshot.value(), selected);
  const auto second = captions::buildTimelineCutProposal(snapshot.value(), selected);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(first.value().timeline_cuts.has_value());
  ASSERT_TRUE(second.value().timeline_cuts.has_value());
  ASSERT_EQ(first.value().timeline_cuts->tracks.size(),
            second.value().timeline_cuts->tracks.size());
  ASSERT_EQ(first.value().timeline_cuts->tracks.size(), 2U);
  const auto& audio = first.value().timeline_cuts->tracks[0].clips;
  const auto& video = first.value().timeline_cuts->tracks[1].clips;
  const auto& second_audio = second.value().timeline_cuts->tracks[0].clips;
  ASSERT_EQ(audio.size(), video.size());
  ASSERT_EQ(audio.size(), second_audio.size());
  ASSERT_GE(audio.size(), 2U);
  EXPECT_NE(audio.front().linked_group,
            snapshot.value().sequence().tracks[0].clips.front().linked_group);
  for (std::size_t index = 0; index < audio.size(); ++index) {
    EXPECT_EQ(audio[index].linked_group, video[index].linked_group);
    EXPECT_EQ(audio[index].timeline_range, second_audio[index].timeline_range);
  }
  EXPECT_EQ(audio.back().timeline_range.start, edit::Time(10, 1));
}

TEST(TranscriptProposal, RejectsLockedAffectedTrackAndSupportsFullRemoval) {
  edit::TimelineEditor locked_editor(project_with_linked_av_tracks(true));
  const auto locked_snapshot = locked_editor.snapshot(
      locked_editor.projectAt(edit::Revision{})->sequences.front().id, edit::Revision{});
  ASSERT_TRUE(locked_snapshot);
  const auto selected = edit::TimeRange(edit::Time(2, 1), edit::Time(1, 1));
  const auto rejected = captions::buildTimelineCutProposal(locked_snapshot.value(), {&selected, 1});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code, captions::ProposalErrorCode::InvalidSnapshot);

  edit::TimelineEditor editor(project_with_tracks());
  const auto snapshot =
      editor.snapshot(editor.projectAt(edit::Revision{})->sequences.front().id, edit::Revision{});
  ASSERT_TRUE(snapshot);
  const auto all = edit::TimeRange(edit::Time(0, 1), edit::Time(20, 1));
  const auto proposal = captions::buildTimelineCutProposal(snapshot.value(), {&all, 1});
  ASSERT_TRUE(proposal);
  ASSERT_TRUE(proposal.value().timeline_cuts.has_value());
  ASSERT_TRUE(proposal.value().timeline_cuts->tracks.front().clips.empty());
}

TEST(TranscriptProposal, DropsTransitionWhenCutOverlapsTransitionRange) {
  auto project = project_with_tracks();
  auto& sequence = project.sequences.front();
  auto clip = sequence.tracks.front().clips.front();
  auto incoming = sequence.tracks.front().clips.back();
  clip.kind = edit::ClipKind::Video;
  incoming.kind = edit::ClipKind::Video;
  clip.id = edit::EntityId::generate();
  incoming.id = edit::EntityId::generate();
  incoming.timeline_range = edit::TimeRange(edit::Time(10, 1), edit::Time(6, 1));
  incoming.source_range = incoming.timeline_range;
  edit::Track video_track;
  video_track.kind = edit::TrackKind::Video;
  video_track.clips = {clip, incoming};
  sequence.tracks.push_back(video_track);
  edit::Transition transition;
  transition.outgoing_clip_id = clip.id;
  transition.incoming_clip_id = incoming.id;
  transition.range = edit::TimeRange(edit::Time(9, 1), edit::Time(2, 1));
  sequence.transitions.push_back(transition);
  edit::TimelineEditor editor(project);
  const auto snapshot = editor.snapshot(sequence.id, edit::Revision{});
  ASSERT_TRUE(snapshot);
  const auto selected = edit::TimeRange(edit::Time(9, 1), edit::Time(1, 1));
  const auto proposal = captions::buildTimelineCutProposal(snapshot.value(), {&selected, 1});
  ASSERT_TRUE(proposal);
  ASSERT_TRUE(proposal.value().timeline_cuts.has_value());
  ASSERT_TRUE(proposal.value().timeline_cuts->transitions.has_value());
  EXPECT_TRUE(proposal.value().timeline_cuts->transitions->empty());

  const auto result = editor.apply(edit::EditCommand{*proposal.value().timeline_cuts, {}},
                                   proposal.value().base_revision);
  ASSERT_TRUE(result);
  const auto after = editor.snapshot(sequence.id, result.value());
  ASSERT_TRUE(after);
  EXPECT_TRUE(after.value().sequence().transitions.empty());
}

} // namespace
