// SPDX-License-Identifier: MPL-2.0
#include "video_editor/project_codec/project_codec.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>

namespace video_editor::project_codec {
namespace {

[[nodiscard]] ProjectBytes bytes(std::initializer_list<std::uint8_t> values) {
  ProjectBytes result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

[[nodiscard]] edit::Effect makeUnknownEffect() {
  edit::Effect effect;
  effect.type = "vendor.future.super_glow";
  effect.version = 17;
  effect.enabled = false;
  effect.known = false;
  effect.opaque_payload = {0x00, 0x01, 0x7F, 0x80, 0xFF};

  edit::EffectParameter amount;
  amount.id = "amount";
  amount.value = 0.75;
  edit::Keyframe start;
  start.time = edit::Time(0, 30'000);
  start.value = 0.25;
  start.interpolation = edit::KeyframeInterpolation::Bezier;
  start.incoming_control = {-0.2, 0.0};
  start.outgoing_control = {0.2, 0.5};
  edit::Keyframe end;
  end.time = edit::Time(30'000, 30'000);
  end.value = 0.75;
  end.interpolation = edit::KeyframeInterpolation::Linear;
  amount.keyframes = {start, end};
  effect.parameters.emplace(amount.id, amount);

  edit::EffectParameter iterations;
  iterations.id = "iterations";
  iterations.value = std::int64_t{4};
  effect.parameters.emplace(iterations.id, iterations);

  edit::EffectParameter bypass;
  bypass.id = "bypass";
  bypass.value = false;
  effect.parameters.emplace(bypass.id, bypass);

  edit::EffectParameter label;
  label.id = "label";
  label.value = std::string("Creator glow");
  effect.parameters.emplace(label.id, label);

  edit::EffectParameter offset;
  offset.id = "offset";
  offset.value = edit::Time(-1001, 30'000);
  effect.parameters.emplace(offset.id, offset);

  edit::EffectParameter center;
  center.id = "center";
  center.value = edit::Vec2{0.25, 0.75};
  effect.parameters.emplace(center.id, center);

  edit::EffectParameter tint;
  tint.id = "tint";
  tint.value = edit::ColorRgba{0.1, 0.2, 0.3, 0.9};
  effect.parameters.emplace(tint.id, tint);
  return effect;
}

[[nodiscard]] edit::Project makeComplexProject() {
  edit::Project project;
  project.name = "Launch video — 日本語";
  project.metadata.emplace("application", "VideoEditor");
  project.metadata.emplace("workspace", "Audio & Captions");

  edit::Asset asset;
  asset.name = "A-roll.mov";
  asset.source_uri = "file:///media/A-roll.mov";
  asset.fingerprint = "sha256:0123456789abcdef";
  asset.duration = edit::Time(3'603'600, 30'000);
  asset.has_video = true;
  asset.has_audio = true;
  asset.width = 3840;
  asset.height = 2160;
  asset.nominal_frame_rate = edit::Rate(30'000, 1'001);
  asset.audio_sample_rate = 48'000;
  asset.audio_channels = 2;
  asset.metadata.emplace("color_primaries", "bt709");
  asset.metadata.emplace("transfer", "bt709");
  project.assets.push_back(asset);

  edit::Sequence sequence;
  sequence.name = "Episode 1";
  sequence.frame_rate = edit::Rate(30'000, 1'001);
  sequence.width = 1920;
  sequence.height = 1080;
  sequence.audio_sample_rate = 48'000;

  edit::Track video;
  video.name = "V1 — Primary";
  video.kind = edit::TrackKind::Video;
  edit::Clip video_clip;
  video_clip.asset_id = asset.id;
  video_clip.kind = edit::ClipKind::Video;
  video_clip.name = "Opening";
  video_clip.timeline_range =
      edit::TimeRange(edit::Time(0, 30'000), edit::Time(300'000, 30'000));
  video_clip.source_range =
      edit::TimeRange(edit::Time(30'030, 30'000), edit::Time(300'000, 30'000));
  video_clip.playback_rate = edit::Rate(1, 1);
  video_clip.linked_group = edit::EntityId::generate();
  video_clip.transform.position = {32.5, -18.25};
  video_clip.transform.scale = {1.1, 1.1};
  video_clip.transform.rotation_degrees = 1.25;
  video_clip.transform.anchor_x = 0.5;
  video_clip.transform.anchor_y = 0.5;
  video_clip.transform.crop_left = 0.01;
  video_clip.transform.crop_top = 0.02;
  video_clip.transform.crop_right = 0.03;
  video_clip.transform.crop_bottom = 0.04;
  video_clip.transform.opacity = 0.85;
  video_clip.blend_mode = edit::BlendMode::Screen;
  video_clip.audio_gain_db = -3.25;
  video_clip.audio_pan = -0.2;
  video_clip.fade_in = edit::Time(15'000, 30'000);
  video_clip.fade_out = edit::Time(30'000, 30'000);
  video_clip.effects.push_back(makeUnknownEffect());
  video.clips.push_back(video_clip);

  edit::Clip title;
  title.kind = edit::ClipKind::Title;
  title.name = "Chapter title";
  title.timeline_range = edit::TimeRange(edit::Time(300'000, 30'000),
                                         edit::Time(90'000, 30'000));
  title.source_range =
      edit::TimeRange(edit::Time{}, edit::Time(90'000, 30'000));
  title.playback_rate = edit::Rate(1, 1);
  title.transform.opacity = 1.0;
  video.clips.push_back(title);

  edit::Effect track_effect;
  track_effect.type = "builtin.color.primary";
  track_effect.version = 2;
  track_effect.known = true;
  video.effects.push_back(track_effect);
  sequence.tracks.push_back(video);

  edit::Track audio;
  audio.name = "A1 Dialogue";
  audio.kind = edit::TrackKind::Audio;
  audio.muted = false;
  audio.solo = true;
  edit::Clip audio_clip;
  audio_clip.asset_id = asset.id;
  audio_clip.kind = edit::ClipKind::Audio;
  audio_clip.name = "Dialogue";
  audio_clip.timeline_range = edit::TimeRange(edit::Time(0, 48'000),
                                               edit::Time(624'000, 48'000));
  audio_clip.source_range = edit::TimeRange(edit::Time(48'048, 48'000),
                                             edit::Time(624'000, 48'000));
  audio_clip.playback_rate = edit::Rate(1, 1);
  audio_clip.audio_gain_db = 2.0;
  audio_clip.fade_in = edit::Time(2'400, 48'000);
  audio_clip.fade_out = edit::Time(4'800, 48'000);
  audio.clips.push_back(audio_clip);
  sequence.tracks.push_back(audio);

  edit::Track captions_track;
  captions_track.name = "English";
  captions_track.kind = edit::TrackKind::Caption;
  sequence.tracks.push_back(captions_track);

  edit::Marker marker;
  marker.range = edit::TimeRange(edit::Time(150'150, 30'000), edit::Time{});
  marker.label = "Music hit";
  marker.color = {0.95, 0.4, 0.1, 1.0};
  sequence.markers.push_back(marker);

  edit::Caption caption;
  caption.range = edit::TimeRange(edit::Time(48'000, 48'000),
                                  edit::Time(144'000, 48'000));
  caption.text = "Welcome to the show!";
  caption.language = "en-GB";
  caption.style.font_family = "Inter";
  caption.style.font_size = 52.0;
  caption.style.text_color = {1.0, 0.98, 0.9, 1.0};
  caption.style.background_color = {0.0, 0.0, 0.0, 0.65};
  caption.style.bold = true;
  sequence.captions.push_back(caption);

  project.sequences.push_back(sequence);
  return project;
}

TEST(ProjectCodecTest, ComplexProjectRoundTripsExactly) {
  const auto original = makeComplexProject();
  const auto encoded = serialize_project(original);
  ASSERT_FALSE(encoded.empty());

  auto decoded = deserialize_project(encoded);
  ASSERT_TRUE(decoded) << (decoded ? "" : decoded.error().message);
  EXPECT_EQ(decoded.value(), original);
}

TEST(ProjectCodecTest, SerializationIsDeterministic) {
  const auto project = makeComplexProject();
  const auto first = serialize_project(project);
  const auto second = serialize_project(project);
  EXPECT_EQ(first, second);

  auto decoded = deserialize_project(first);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(serialize_project(decoded.value()), first);
}

TEST(ProjectCodecTest, UnknownEffectPayloadAndTypedValuesRoundTrip) {
  const auto project = makeComplexProject();
  auto decoded = deserialize_project(serialize_project(project));
  ASSERT_TRUE(decoded);
  const auto& effect = decoded.value().sequences[0].tracks[0].clips[0].effects[0];
  ASSERT_FALSE(effect.known);
  EXPECT_FALSE(effect.enabled);
  EXPECT_EQ(effect.type, "vendor.future.super_glow");
  EXPECT_EQ(effect.version, 17U);
  EXPECT_EQ(effect.opaque_payload,
            (std::vector<std::uint8_t>{0x00, 0x01, 0x7F, 0x80, 0xFF}));
  EXPECT_TRUE(std::holds_alternative<std::int64_t>(
      effect.parameters.at("iterations").value));
  EXPECT_TRUE(std::holds_alternative<bool>(effect.parameters.at("bypass").value));
  EXPECT_TRUE(std::holds_alternative<std::string>(
      effect.parameters.at("label").value));
  EXPECT_TRUE(std::holds_alternative<edit::Time>(
      effect.parameters.at("offset").value));
  EXPECT_TRUE(std::holds_alternative<edit::Vec2>(effect.parameters.at("center").value));
  EXPECT_TRUE(
      std::holds_alternative<edit::ColorRgba>(effect.parameters.at("tint").value));
}

TEST(ProjectCodecTest, RejectsMalformedAndMissingVersionData) {
  const auto malformed = deserialize_project(bytes({0xFF, 0xFF, 0xFF}));
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().code, CodecErrorCode::MalformedProtobuf);

  const auto empty = deserialize_project(ProjectBytes{});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().code, CodecErrorCode::MissingField);
  EXPECT_EQ(empty.error().field_path, "snapshot.schema_version");
}

TEST(ProjectCodecTest, RejectsFutureSchemaAndReaderVersions) {
  // schema_version = 2, minimum_reader_version = 1
  const auto future_schema = deserialize_project(bytes({0x08, 0x02, 0x10, 0x01}));
  ASSERT_FALSE(future_schema);
  EXPECT_EQ(future_schema.error().code,
            CodecErrorCode::UnsupportedSchemaVersion);

  // schema_version = 1, minimum_reader_version = 2
  const auto future_reader = deserialize_project(bytes({0x08, 0x01, 0x10, 0x02}));
  ASSERT_FALSE(future_reader);
  EXPECT_EQ(future_reader.error().code,
            CodecErrorCode::UnsupportedMinimumReaderVersion);
}

TEST(ProjectCodecTest, RejectsUnknownFieldsAtTheDeclaredCurrentVersion) {
  // The fourth root field is unknown to schema v1. It must not be silently
  // dropped, even though protobuf itself can parse it.
  const auto unknown =
      deserialize_project(bytes({0x08, 0x01, 0x10, 0x01, 0x20, 0x01}));
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().code, CodecErrorCode::InvalidField);
  EXPECT_EQ(unknown.error().field_path, "snapshot");
}

TEST(ProjectCodecTest, RefusesToSerializeInvalidProjectState) {
  auto project = makeComplexProject();
  project.assets[0].id = project.id;
  try {
    (void)serialize_project(project);
    FAIL() << "expected CodecException";
  } catch (const CodecException& exception) {
    EXPECT_EQ(exception.error().code, CodecErrorCode::InvalidProject);
    EXPECT_FALSE(exception.error().message.empty());
  }
}

TEST(ProjectCodecTest, PreservesRawRationalRepresentationsAndUuidBytes) {
  auto project = makeComplexProject();
  project.assets[0].duration = edit::Time(6, 12);
  // Keep every clip source within the shortened asset for strict validation.
  project.sequences.clear();

  auto decoded = deserialize_project(serialize_project(project));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().assets[0].duration.value(), 6);
  EXPECT_EQ(decoded.value().assets[0].duration.timescale(), 12U);
  EXPECT_EQ(decoded.value().id.bytes(), project.id.bytes());
}

}  // namespace
}  // namespace video_editor::project_codec
