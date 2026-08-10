// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <random>
#include <variant>
#include <vector>

namespace video_editor::edit {
namespace {

struct PropertyFixture final {
  Project project;
  EntityId sequence_id;
  EntityId video_track_id;
  EntityId audio_track_id;
  EntityId video_clip_id;
  EntityId audio_clip_id;
};

[[nodiscard]] PropertyFixture makePropertyFixture() {
  PropertyFixture result;
  Asset asset;
  asset.name = "media.mov";
  asset.source_uri = "memory://media";
  asset.duration = Time(20, 1);
  asset.has_video = true;
  asset.has_audio = true;
  result.project.assets.push_back(asset);

  Sequence sequence;
  result.sequence_id = sequence.id;

  Track video_track;
  video_track.kind = TrackKind::Video;
  result.video_track_id = video_track.id;
  Clip video_clip;
  video_clip.asset_id = asset.id;
  video_clip.kind = ClipKind::Video;
  video_clip.timeline_range = TimeRange(Time{}, Time(10, 1));
  video_clip.source_range = video_clip.timeline_range;
  result.video_clip_id = video_clip.id;
  video_track.clips.push_back(video_clip);

  Track audio_track;
  audio_track.kind = TrackKind::Audio;
  result.audio_track_id = audio_track.id;
  Clip audio_clip;
  audio_clip.asset_id = asset.id;
  audio_clip.kind = ClipKind::Audio;
  audio_clip.timeline_range = TimeRange(Time{}, Time(10, 1));
  audio_clip.source_range = audio_clip.timeline_range;
  result.audio_clip_id = audio_clip.id;
  audio_track.clips.push_back(audio_clip);

  sequence.tracks = {video_track, audio_track};
  result.project.sequences.push_back(sequence);
  return result;
}

[[nodiscard]] const Clip& currentClip(TimelineEditor& editor, const EntityId sequence_id,
                                      const EntityId clip_id) {
  auto snapshot = editor.snapshot(sequence_id, editor.revision());
  EXPECT_TRUE(snapshot) << (snapshot ? "" : snapshot.error().message);
  const auto* clip = snapshot ? snapshot.value().findClip(clip_id) : nullptr;
  EXPECT_NE(clip, nullptr);
  return *clip;
}

TEST(ClipPropertiesTest, AppendsOperationsWithoutChangingExistingVariantOrdinals) {
  EXPECT_EQ(EditOperation{AddAssetCommand{}}.index(), 0U);
  EXPECT_EQ(EditOperation{SetClipEffectParameterCommand{}}.index(), 22U);
  EXPECT_EQ(EditOperation{SetSequenceFormatCommand{}}.index(), 23U);
  EXPECT_EQ(EditOperation{SetClipTransformCommand{}}.index(), 24U);
  EXPECT_EQ(EditOperation{SetClipBlendModeCommand{}}.index(), 25U);
  EXPECT_EQ(EditOperation{SetClipAudioPropertiesCommand{}}.index(), 26U);
  EXPECT_EQ(EditOperation{SetTrackAudioStateCommand{}}.index(), 27U);
  EXPECT_EQ(std::variant_size_v<EditOperation>, 28U);
}

TEST(ClipPropertiesTest, AppliesAndRoundTripsTypedPropertiesAsExactRevisions) {
  auto fixture = makePropertyFixture();
  const auto initial = fixture.project;
  TimelineEditor editor(initial);

  Transform transform;
  transform.position = {320.25, -180.5};
  transform.scale = {-1.5, 0.75};
  transform.rotation_degrees = 37.5;
  transform.anchor_x = 0.25;
  transform.anchor_y = 0.8;
  transform.crop_left = 0.1;
  transform.crop_top = 0.2;
  transform.crop_right = 0.3;
  transform.crop_bottom = 0.1;
  transform.opacity = 0.625;

  auto transformed = editor.apply(
      EditCommand{SetClipTransformCommand{fixture.sequence_id, fixture.video_clip_id, transform},
                  {}},
      Revision{0});
  ASSERT_TRUE(transformed) << (transformed ? "" : transformed.error().message);
  EXPECT_EQ(transformed.value(), Revision{1});
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.video_clip_id).transform, transform);

  auto blended =
      editor.apply(EditCommand{SetClipBlendModeCommand{fixture.sequence_id, fixture.video_clip_id,
                                                       BlendMode::Overlay},
                               {}},
                   transformed.value());
  ASSERT_TRUE(blended) << (blended ? "" : blended.error().message);
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.video_clip_id).blend_mode,
            BlendMode::Overlay);

  const SetClipAudioPropertiesCommand audio{
      fixture.sequence_id, fixture.audio_clip_id, -7.25, 0.35, Time(3, 2), Time(5, 2)};
  auto mixed = editor.apply(EditCommand{audio, {}}, blended.value());
  ASSERT_TRUE(mixed) << (mixed ? "" : mixed.error().message);
  const auto& changed_audio = currentClip(editor, fixture.sequence_id, fixture.audio_clip_id);
  EXPECT_DOUBLE_EQ(changed_audio.audio_gain_db, -7.25);
  EXPECT_DOUBLE_EQ(changed_audio.audio_pan, 0.35);
  EXPECT_EQ(changed_audio.fade_in, Time(3, 2));
  EXPECT_EQ(changed_audio.fade_out, Time(5, 2));

  auto undo_audio = editor.undo(mixed.value());
  ASSERT_TRUE(undo_audio);
  auto undo_blend = editor.undo(undo_audio.value());
  ASSERT_TRUE(undo_blend);
  auto undo_transform = editor.undo(undo_blend.value());
  ASSERT_TRUE(undo_transform);
  EXPECT_EQ(*editor.projectAt(undo_transform.value()), initial);

  auto redo_transform = editor.redo(undo_transform.value());
  ASSERT_TRUE(redo_transform);
  auto redo_blend = editor.redo(redo_transform.value());
  ASSERT_TRUE(redo_blend);
  auto redo_audio = editor.redo(redo_blend.value());
  ASSERT_TRUE(redo_audio);
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.video_clip_id).transform, transform);
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.video_clip_id).blend_mode,
            BlendMode::Overlay);
  EXPECT_DOUBLE_EQ(currentClip(editor, fixture.sequence_id, fixture.audio_clip_id).audio_gain_db,
                   -7.25);

  const auto names = editor.history();
  ASSERT_EQ(names.size(), 3U);
  EXPECT_EQ(names[0].command_name, "Set clip transform");
  EXPECT_EQ(names[1].command_name, "Set clip blend mode");
  EXPECT_EQ(names[2].command_name, "Set clip audio properties");
}

TEST(ClipPropertiesTest, RejectsEveryInvalidTransformWithoutMutation) {
  auto fixture = makePropertyFixture();
  TimelineEditor editor(fixture.project);
  std::vector<Transform> invalid;

  Transform value;
  value.position.x = std::numeric_limits<double>::quiet_NaN();
  invalid.push_back(value);
  value = {};
  value.position.y = 1'000'000.1;
  invalid.push_back(value);
  value = {};
  value.scale.x = 0.0;
  invalid.push_back(value);
  value = {};
  value.scale.y = std::numeric_limits<double>::infinity();
  invalid.push_back(value);
  value = {};
  value.rotation_degrees = 36'000.1;
  invalid.push_back(value);
  value = {};
  value.anchor_x = -0.01;
  invalid.push_back(value);
  value = {};
  value.crop_left = 0.6;
  value.crop_right = 0.4;
  invalid.push_back(value);
  value = {};
  value.crop_bottom = std::numeric_limits<double>::quiet_NaN();
  invalid.push_back(value);
  value = {};
  value.opacity = 1.01;
  invalid.push_back(value);

  for (const auto& transform : invalid) {
    const auto result = editor.apply(
        EditCommand{SetClipTransformCommand{fixture.sequence_id, fixture.video_clip_id, transform},
                    {}},
        Revision{0});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, EditErrorCode::InvalidArgument);
    EXPECT_EQ(editor.revision(), Revision{0});
    EXPECT_FALSE(editor.canUndo());
  }
}

TEST(ClipPropertiesTest, RejectsInvalidAudioValuesKindsAndLockedTracksAtomically) {
  auto fixture = makePropertyFixture();
  TimelineEditor editor(fixture.project);
  const std::vector<SetClipAudioPropertiesCommand> invalid{
      {fixture.sequence_id, fixture.audio_clip_id, std::numeric_limits<double>::quiet_NaN(), 0.0,
       Time{}, Time{}},
      {fixture.sequence_id, fixture.audio_clip_id, -96.01, 0.0, Time{}, Time{}},
      {fixture.sequence_id, fixture.audio_clip_id, 24.01, 0.0, Time{}, Time{}},
      {fixture.sequence_id, fixture.audio_clip_id, 0.0, std::numeric_limits<double>::infinity(),
       Time{}, Time{}},
      {fixture.sequence_id, fixture.audio_clip_id, 0.0, 1.01, Time{}, Time{}},
      {fixture.sequence_id, fixture.audio_clip_id, 0.0, 0.0, Time(-1, 1), Time{}},
      {fixture.sequence_id, fixture.audio_clip_id, 0.0, 0.0, Time(6, 1), Time(5, 1)},
  };
  for (const auto& command : invalid) {
    const auto result = editor.apply(EditCommand{command, {}}, Revision{0});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, EditErrorCode::InvalidArgument);
    EXPECT_EQ(editor.revision(), Revision{0});
  }

  const auto audio_on_video = editor.apply(
      EditCommand{SetClipAudioPropertiesCommand{fixture.sequence_id, fixture.video_clip_id}, {}},
      Revision{0});
  ASSERT_FALSE(audio_on_video);
  EXPECT_EQ(audio_on_video.error().code, EditErrorCode::InvalidTrackKind);
  const auto transform_on_audio = editor.apply(
      EditCommand{SetClipTransformCommand{fixture.sequence_id, fixture.audio_clip_id, {}}, {}},
      Revision{0});
  ASSERT_FALSE(transform_on_audio);
  EXPECT_EQ(transform_on_audio.error().code, EditErrorCode::InvalidTrackKind);
  const auto blend_on_audio =
      editor.apply(EditCommand{SetClipBlendModeCommand{fixture.sequence_id, fixture.audio_clip_id,
                                                       BlendMode::Multiply},
                               {}},
                   Revision{0});
  ASSERT_FALSE(blend_on_audio);
  EXPECT_EQ(blend_on_audio.error().code, EditErrorCode::InvalidTrackKind);
  const auto invalid_blend =
      editor.apply(EditCommand{SetClipBlendModeCommand{fixture.sequence_id, fixture.video_clip_id,
                                                       static_cast<BlendMode>(255)},
                               {}},
                   Revision{0});
  ASSERT_FALSE(invalid_blend);
  EXPECT_EQ(invalid_blend.error().code, EditErrorCode::InvalidArgument);

  fixture.project.sequences[0].tracks[0].locked = true;
  TimelineEditor locked_editor(fixture.project);
  const auto locked = locked_editor.apply(
      EditCommand{SetClipTransformCommand{fixture.sequence_id, fixture.video_clip_id, {}}, {}},
      Revision{0});
  ASSERT_FALSE(locked);
  EXPECT_EQ(locked.error().code, EditErrorCode::TrackLocked);
  EXPECT_EQ(locked_editor.revision(), Revision{0});
}

TEST(ClipPropertiesTest, HonorsEntityAndRevisionContracts) {
  auto fixture = makePropertyFixture();
  TimelineEditor editor(fixture.project);
  const auto stale = editor.apply(
      EditCommand{SetClipTransformCommand{fixture.sequence_id, fixture.video_clip_id, {}}, {}},
      Revision{1});
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, EditErrorCode::RevisionConflict);

  const auto missing_sequence = editor.apply(
      EditCommand{SetClipTransformCommand{EntityId::generate(), fixture.video_clip_id, {}}, {}},
      Revision{0});
  ASSERT_FALSE(missing_sequence);
  EXPECT_EQ(missing_sequence.error().code, EditErrorCode::EntityNotFound);
  const auto missing_clip = editor.apply(
      EditCommand{SetClipTransformCommand{fixture.sequence_id, EntityId::generate(), {}}, {}},
      Revision{0});
  ASSERT_FALSE(missing_clip);
  EXPECT_EQ(missing_clip.error().code, EditErrorCode::EntityNotFound);
  EXPECT_EQ(editor.revision(), Revision{0});
}

TEST(ClipPropertiesTest, ValidRandomPropertiesCoalesceAndRoundTrip) {
  auto fixture = makePropertyFixture();
  TimelineEditor editor(fixture.project);
  std::mt19937_64 random(0xC1A0BEEF);
  std::uniform_real_distribution<double> position(-10'000.0, 10'000.0);
  std::uniform_real_distribution<double> scale(0.01, 10.0);
  std::uniform_real_distribution<double> rotation(-720.0, 720.0);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::uniform_real_distribution<double> crop(0.0, 0.45);
  std::uniform_real_distribution<double> gain(-96.0, 24.0);
  std::uniform_real_distribution<double> pan(-1.0, 1.0);

  Transform final_transform;
  for (int iteration = 0; iteration < 100; ++iteration) {
    Transform transform;
    transform.position = {position(random), position(random)};
    transform.scale = {(iteration % 2 == 0 ? 1.0 : -1.0) * scale(random), scale(random)};
    transform.rotation_degrees = rotation(random);
    transform.anchor_x = unit(random);
    transform.anchor_y = unit(random);
    transform.crop_left = crop(random);
    transform.crop_right = crop(random);
    transform.crop_top = crop(random);
    transform.crop_bottom = crop(random);
    transform.opacity = unit(random);
    const auto result = editor.apply(
        EditCommand{SetClipTransformCommand{fixture.sequence_id, fixture.video_clip_id, transform},
                    "transform-drag"},
        editor.revision());
    ASSERT_TRUE(result) << (result ? "" : result.error().message);
    final_transform = transform;
  }
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.video_clip_id).transform,
            final_transform);
  ASSERT_EQ(editor.history().size(), 1U);
  EXPECT_EQ(editor.history().front().command_count, 100U);
  auto undo_transform = editor.undo(editor.revision());
  ASSERT_TRUE(undo_transform);
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.video_clip_id).transform, Transform{});
  auto redo_transform = editor.redo(undo_transform.value());
  ASSERT_TRUE(redo_transform);
  EXPECT_EQ(currentClip(editor, fixture.sequence_id, fixture.video_clip_id).transform,
            final_transform);

  double final_gain = 0.0;
  double final_pan = 0.0;
  for (int iteration = 0; iteration < 100; ++iteration) {
    final_gain = gain(random);
    final_pan = pan(random);
    const auto result = editor.apply(
        EditCommand{SetClipAudioPropertiesCommand{fixture.sequence_id, fixture.audio_clip_id,
                                                  final_gain, final_pan, Time(1, 1), Time(2, 1)},
                    "audio-drag"},
        editor.revision());
    ASSERT_TRUE(result) << (result ? "" : result.error().message);
  }
  const auto& audio = currentClip(editor, fixture.sequence_id, fixture.audio_clip_id);
  EXPECT_DOUBLE_EQ(audio.audio_gain_db, final_gain);
  EXPECT_DOUBLE_EQ(audio.audio_pan, final_pan);
  ASSERT_EQ(editor.history().size(), 2U);
  EXPECT_EQ(editor.history().back().command_count, 100U);
}

} // namespace
} // namespace video_editor::edit
