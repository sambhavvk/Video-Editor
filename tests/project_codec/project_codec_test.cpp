// SPDX-License-Identifier: MPL-2.0
#include "project_snapshot.pb.h"
#include "video_editor/project_codec/project_codec.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

void appendVarint(std::vector<std::uint8_t>& output, std::uint64_t value) {
  do {
    std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7FU);
    value >>= 7U;
    if (value != 0) {
      byte = static_cast<std::uint8_t>(byte | 0x80U);
    }
    output.push_back(byte);
  } while (value != 0);
}

void appendLengthDelimited(std::vector<std::uint8_t>& output,
                           const std::vector<std::uint8_t>& data) {
  appendVarint(output, data.size());
  output.insert(output.end(), data.begin(), data.end());
}

void appendFieldVarint(std::vector<std::uint8_t>& output, const std::uint32_t field_number,
                       const std::uint64_t value) {
  appendVarint(output, (static_cast<std::uint64_t>(field_number) << 3U) | 0U);
  appendVarint(output, value);
}

void appendFieldSInt64(std::vector<std::uint8_t>& output, const std::uint32_t field_number,
                       const std::int64_t value) {
  const auto zig_zag = static_cast<std::uint64_t>(
      (value << 1) ^ (value >> (std::numeric_limits<std::int64_t>::digits)));
  appendFieldVarint(output, field_number, zig_zag);
}

void appendFieldDouble(std::vector<std::uint8_t>& output, const std::uint32_t field_number,
                       const double value) {
  static_assert(sizeof(double) == sizeof(std::uint64_t));
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  appendVarint(output, (static_cast<std::uint64_t>(field_number) << 3U) | 1U);
  for (std::size_t index = 0; index < sizeof(bits); ++index) {
    output.push_back(static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xFFU));
  }
}

void appendFieldString(std::vector<std::uint8_t>& output, const std::uint32_t field_number,
                       const std::string_view value) {
  appendVarint(output, (static_cast<std::uint64_t>(field_number) << 3U) | 2U);
  appendVarint(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

void appendFieldMessage(std::vector<std::uint8_t>& output, const std::uint32_t field_number,
                        const std::vector<std::uint8_t>& message) {
  appendVarint(output, (static_cast<std::uint64_t>(field_number) << 3U) | 2U);
  appendLengthDelimited(output, message);
}

[[nodiscard]] std::vector<std::uint8_t> makeEntityIdMessage(const std::uint8_t fill) {
  std::vector<std::uint8_t> entity_id;
  appendFieldMessage(entity_id, 1, std::vector<std::uint8_t>(16U, fill));
  return entity_id;
}

[[nodiscard]] ProjectBytes packBytes(const std::vector<std::uint8_t>& source) {
  ProjectBytes result;
  result.reserve(source.size());
  for (const auto value : source) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

[[nodiscard]] std::vector<std::uint8_t> makeTimeMessage(const std::int64_t value,
                                                        const std::uint32_t timescale) {
  std::vector<std::uint8_t> time;
  appendFieldSInt64(time, 1, value);
  appendFieldVarint(time, 2, timescale);
  return time;
}

[[nodiscard]] std::vector<std::uint8_t> makeTimeRangeMessage(const std::int64_t start_value,
                                                             const std::int64_t duration_value,
                                                             const std::uint32_t timescale) {
  std::vector<std::uint8_t> range;
  appendFieldMessage(range, 1, makeTimeMessage(start_value, timescale));
  appendFieldMessage(range, 2, makeTimeMessage(duration_value, timescale));
  return range;
}

[[nodiscard]] std::vector<std::uint8_t> makeRateMessage(const std::uint32_t numerator,
                                                        const std::uint32_t denominator) {
  std::vector<std::uint8_t> rate;
  appendFieldVarint(rate, 1, numerator);
  appendFieldVarint(rate, 2, denominator);
  return rate;
}

[[nodiscard]] std::vector<std::uint8_t> makeVec2Message(const double x, const double y) {
  std::vector<std::uint8_t> vec2;
  appendFieldDouble(vec2, 1, x);
  appendFieldDouble(vec2, 2, y);
  return vec2;
}

[[nodiscard]] std::vector<std::uint8_t> makeTransformMessage() {
  std::vector<std::uint8_t> transform;
  appendFieldMessage(transform, 1, makeVec2Message(0.0, 0.0));
  appendFieldMessage(transform, 2, makeVec2Message(1.0, 1.0));
  appendFieldDouble(transform, 10, 1.0);
  return transform;
}

[[nodiscard]] ProjectBytes makeGenuineV1TitleUpgradeSnapshot() {
  constexpr std::uint32_t kTimescale = 30'000U;

  std::vector<std::uint8_t> asset;
  appendFieldMessage(asset, 1, makeEntityIdMessage(0x21U));
  appendFieldString(asset, 2, "source.mov");
  appendFieldMessage(asset, 5, makeTimeMessage(600'000, kTimescale));
  appendFieldVarint(asset, 6, 1);
  appendFieldVarint(asset, 7, 1);
  appendFieldVarint(asset, 8, 1920);
  appendFieldVarint(asset, 9, 1080);
  appendFieldVarint(asset, 11, 48'000);
  appendFieldVarint(asset, 12, 2);

  std::vector<std::uint8_t> title_clip;
  appendFieldMessage(title_clip, 1, makeEntityIdMessage(0x31U));
  appendFieldMessage(title_clip, 2, makeEntityIdMessage(0x00U));
  appendFieldVarint(title_clip, 3, 3);
  appendFieldString(title_clip, 4, "Upgraded title text");
  appendFieldMessage(title_clip, 5, makeTimeRangeMessage(0, 90'000, kTimescale));
  appendFieldMessage(title_clip, 6, makeTimeRangeMessage(0, 90'000, kTimescale));
  appendFieldMessage(title_clip, 7, makeRateMessage(1, 1));
  appendFieldMessage(title_clip, 10, makeTransformMessage());
  appendFieldVarint(title_clip, 11, 1);
  appendFieldMessage(title_clip, 14, makeTimeMessage(0, kTimescale));
  appendFieldMessage(title_clip, 15, makeTimeMessage(0, kTimescale));

  std::vector<std::uint8_t> audio_clip;
  appendFieldMessage(audio_clip, 1, makeEntityIdMessage(0x32U));
  appendFieldMessage(audio_clip, 2, makeEntityIdMessage(0x21U));
  appendFieldVarint(audio_clip, 3, 2);
  appendFieldString(audio_clip, 4, "Dialogue");
  appendFieldMessage(audio_clip, 5, makeTimeRangeMessage(0, 144'000, 48'000U));
  appendFieldMessage(audio_clip, 6, makeTimeRangeMessage(0, 144'000, 48'000U));
  appendFieldMessage(audio_clip, 7, makeRateMessage(1, 1));
  appendFieldMessage(audio_clip, 10, makeTransformMessage());
  appendFieldVarint(audio_clip, 11, 1);
  appendFieldMessage(audio_clip, 14, makeTimeMessage(0, 48'000U));
  appendFieldMessage(audio_clip, 15, makeTimeMessage(0, 48'000U));

  std::vector<std::uint8_t> video_track;
  appendFieldMessage(video_track, 1, makeEntityIdMessage(0x41U));
  appendFieldVarint(video_track, 2, 1);
  appendFieldString(video_track, 3, "V1");
  appendFieldMessage(video_track, 7, title_clip);

  std::vector<std::uint8_t> audio_track;
  appendFieldMessage(audio_track, 1, makeEntityIdMessage(0x42U));
  appendFieldVarint(audio_track, 2, 2);
  appendFieldString(audio_track, 3, "A1");
  appendFieldMessage(audio_track, 7, audio_clip);

  std::vector<std::uint8_t> sequence;
  appendFieldMessage(sequence, 1, makeEntityIdMessage(0x51U));
  appendFieldString(sequence, 2, "Sequence 1");
  appendFieldMessage(sequence, 3, makeRateMessage(30'000, 1'001));
  appendFieldVarint(sequence, 4, 1920);
  appendFieldVarint(sequence, 5, 1080);
  appendFieldVarint(sequence, 6, 48'000);
  appendFieldMessage(sequence, 7, video_track);
  appendFieldMessage(sequence, 7, audio_track);

  std::vector<std::uint8_t> project;
  appendFieldMessage(project, 1, makeEntityIdMessage(0x61U));
  appendFieldString(project, 2, "Legacy v1 project");
  appendFieldMessage(project, 3, asset);
  appendFieldMessage(project, 4, sequence);

  std::vector<std::uint8_t> snapshot;
  appendFieldVarint(snapshot, 1, 1);
  appendFieldVarint(snapshot, 2, 1);
  appendFieldMessage(snapshot, 3, project);
  return packBytes(snapshot);
}

[[nodiscard]] ProjectBytes mutateSchemaVersion(ProjectBytes encoded,
                                               const std::uint8_t schema_version) {
  EXPECT_GE(encoded.size(), 2U);
  EXPECT_EQ(encoded[0], std::byte{0x08});
  encoded[1] = static_cast<std::byte>(schema_version);
  return encoded;
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
  video_clip.timeline_range = edit::TimeRange(edit::Time(0, 30'000), edit::Time(300'000, 30'000));
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
  title.timeline_range = edit::TimeRange(edit::Time(300'000, 30'000), edit::Time(90'000, 30'000));
  title.source_range = edit::TimeRange(edit::Time{}, edit::Time(90'000, 30'000));
  title.playback_rate = edit::Rate(1, 1);
  title.transform.opacity = 1.0;
  title.title = edit::Title{
      .text = "Episode 1",
      .font_family = "Inter",
      .font_size = 96.0,
      .foreground_color = {1.0, 1.0, 1.0, 1.0},
      .background_color = {0.0, 0.0, 0.0, 0.25},
      .horizontal_alignment = edit::TitleHorizontalAlignment::Center,
      .bold = true,
      .italic = false,
  };
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
  audio_clip.timeline_range = edit::TimeRange(edit::Time(0, 48'000), edit::Time(624'000, 48'000));
  audio_clip.source_range =
      edit::TimeRange(edit::Time(48'048, 48'000), edit::Time(624'000, 48'000));
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
  caption.range = edit::TimeRange(edit::Time(48'000, 48'000), edit::Time(144'000, 48'000));
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

[[nodiscard]] edit::Project makeSchemaV1CompatibleProject() {
  auto project = makeComplexProject();
  project.sequences[0].tracks[0].clips.erase(project.sequences[0].tracks[0].clips.begin() + 1);
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

TEST(ProjectCodecTest, TrackVisibilityAndTargetingRoundTripAndOldV2Defaults) {
  auto project = makeComplexProject();
  auto& track = project.sequences[0].tracks[0];
  track.visible = false;
  track.targeted = false;
  const auto encoded = serialize_project(project);
  auto decoded = deserialize_project(encoded);
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(decoded.value().sequences[0].tracks[0].visible);
  EXPECT_FALSE(decoded.value().sequences[0].tracks[0].targeted);

  video_editor::persistence::v1::ProjectSnapshot old_v2;
  ASSERT_TRUE(old_v2.ParseFromArray(encoded.data(), static_cast<int>(encoded.size())));
  old_v2.mutable_project()->mutable_sequences(0)->mutable_tracks(0)->clear_visible();
  old_v2.mutable_project()->mutable_sequences(0)->mutable_tracks(0)->clear_targeted();
  std::string old_bytes;
  ASSERT_TRUE(old_v2.SerializeToString(&old_bytes));
  ProjectBytes old_payload;
  old_payload.reserve(old_bytes.size());
  for (const char byte : old_bytes)
    old_payload.push_back(static_cast<std::byte>(byte));
  decoded = deserialize_project(old_payload);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(decoded.value().sequences[0].tracks[0].visible);
  EXPECT_TRUE(decoded.value().sequences[0].tracks[0].targeted);
}

TEST(ProjectCodecTest, SerializationIsDeterministic) {
  const auto project = makeComplexProject();
  const auto first = serialize_project(project);
  const auto second = serialize_project(project);
  EXPECT_EQ(first, second);

  auto decoded = deserialize_project(first);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(serialize_project(decoded.value()), first);
  ASSERT_GE(first.size(), 4U);
  EXPECT_EQ(first[0], std::byte{0x08});
  EXPECT_EQ(first[1], std::byte{0x02});
  EXPECT_EQ(first[2], std::byte{0x10});
  EXPECT_EQ(first[3], std::byte{0x01});
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
  EXPECT_EQ(effect.opaque_payload, (std::vector<std::uint8_t>{0x00, 0x01, 0x7F, 0x80, 0xFF}));
  EXPECT_TRUE(std::holds_alternative<std::int64_t>(effect.parameters.at("iterations").value));
  EXPECT_TRUE(std::holds_alternative<bool>(effect.parameters.at("bypass").value));
  EXPECT_TRUE(std::holds_alternative<std::string>(effect.parameters.at("label").value));
  EXPECT_TRUE(std::holds_alternative<edit::Time>(effect.parameters.at("offset").value));
  EXPECT_TRUE(std::holds_alternative<edit::Vec2>(effect.parameters.at("center").value));
  EXPECT_TRUE(std::holds_alternative<edit::ColorRgba>(effect.parameters.at("tint").value));
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
  // schema_version = 3, minimum_reader_version = 1
  const auto future_schema = deserialize_project(bytes({0x08, 0x03, 0x10, 0x01}));
  ASSERT_FALSE(future_schema);
  EXPECT_EQ(future_schema.error().code, CodecErrorCode::UnsupportedSchemaVersion);

  // schema_version = 2, minimum_reader_version = 3
  const auto future_reader = deserialize_project(bytes({0x08, 0x02, 0x10, 0x03}));
  ASSERT_FALSE(future_reader);
  EXPECT_EQ(future_reader.error().code, CodecErrorCode::UnsupportedMinimumReaderVersion);
}

TEST(ProjectCodecTest, RejectsUnknownFieldsAtTheDeclaredCurrentVersion) {
  // The fourth root field is unknown to schema v2. It must not be silently
  // dropped, even though protobuf itself can parse it.
  const auto unknown = deserialize_project(bytes({0x08, 0x02, 0x10, 0x01, 0x1A, 0x00, 0x20, 0x01}));
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().code, CodecErrorCode::InvalidField);
  EXPECT_EQ(unknown.error().field_path, "snapshot");
}

TEST(ProjectCodecTest, AcceptsDeclaredVersionOneProjectsAndRewritesCanonicalVersionTwo) {
  const auto v2_project = makeSchemaV1CompatibleProject();
  const auto canonical_v2 = serialize_project(v2_project);

  video_editor::persistence::v1::ProjectSnapshot old_v2;
  ASSERT_TRUE(old_v2.ParseFromArray(canonical_v2.data(), static_cast<int>(canonical_v2.size())));
  old_v2.set_schema_version(1);
  for (auto& sequence : *old_v2.mutable_project()->mutable_sequences()) {
    for (auto& track : *sequence.mutable_tracks()) {
      track.clear_visible();
      track.clear_targeted();
    }
  }
  std::string old_bytes;
  ASSERT_TRUE(old_v2.SerializeToString(&old_bytes));
  ProjectBytes declared_v1;
  declared_v1.reserve(old_bytes.size());
  for (const char byte : old_bytes) {
    declared_v1.push_back(static_cast<std::byte>(byte));
  }

  auto decoded = deserialize_project(declared_v1);
  ASSERT_TRUE(decoded) << (decoded ? "" : decoded.error().message);
  EXPECT_EQ(decoded.value(), v2_project);
  EXPECT_EQ(serialize_project(decoded.value()), canonical_v2);
}

TEST(ProjectCodecTest, UpgradesGenuineVersionOneTitleClipAndLeavesMediaClipsTitleless) {
  auto decoded = deserialize_project(makeGenuineV1TitleUpgradeSnapshot());
  ASSERT_TRUE(decoded) << (decoded ? "" : decoded.error().message);

  const auto& sequence = decoded.value().sequences.at(0);
  const auto& title_clip = sequence.tracks.at(0).clips.at(0);
  ASSERT_EQ(title_clip.kind, edit::ClipKind::Title);
  ASSERT_TRUE(title_clip.title.has_value());
  EXPECT_EQ(title_clip.title->text, title_clip.name);

  const auto& media_clip = sequence.tracks.at(1).clips.at(0);
  EXPECT_EQ(media_clip.kind, edit::ClipKind::Audio);
  EXPECT_FALSE(media_clip.title.has_value());

  const auto reserialized = serialize_project(decoded.value());
  ASSERT_GE(reserialized.size(), 4U);
  EXPECT_EQ(reserialized[0], std::byte{0x08});
  EXPECT_EQ(reserialized[1], std::byte{0x02});
  EXPECT_EQ(reserialized[2], std::byte{0x10});
  EXPECT_EQ(reserialized[3], std::byte{0x01});
}

TEST(ProjectCodecTest, RejectsDeclaredVersionOneSnapshotsThatSmuggleVersionTwoFields) {
  std::vector<std::uint8_t> transition_message;
  std::vector<std::uint8_t> sequence_message;
  appendFieldMessage(sequence_message, 10, transition_message);
  std::vector<std::uint8_t> project_message;
  appendFieldMessage(project_message, 1, makeEntityIdMessage(0x11U));
  appendFieldMessage(project_message, 4, sequence_message);

  std::vector<std::uint8_t> snapshot;
  appendFieldVarint(snapshot, 1, 1);
  appendFieldVarint(snapshot, 2, 1);
  appendFieldMessage(snapshot, 3, project_message);

  const auto decoded = deserialize_project(packBytes(snapshot));
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error().code, CodecErrorCode::InvalidField);
  EXPECT_EQ(decoded.error().field_path, "project.sequences[0].transitions[0]");
}

TEST(ProjectCodecTest, RejectsDeclaredVersionOneSnapshotsThatSmuggleTrackPresentationFields) {
  std::vector<std::uint8_t> track_message;
  appendFieldVarint(track_message, 9, 1);
  std::vector<std::uint8_t> sequence_message;
  appendFieldMessage(sequence_message, 7, track_message);
  std::vector<std::uint8_t> project_message;
  appendFieldMessage(project_message, 1, makeEntityIdMessage(0x11U));
  appendFieldMessage(project_message, 4, sequence_message);

  std::vector<std::uint8_t> snapshot;
  appendFieldVarint(snapshot, 1, 1);
  appendFieldVarint(snapshot, 2, 1);
  appendFieldMessage(snapshot, 3, project_message);

  const auto decoded = deserialize_project(packBytes(snapshot));
  ASSERT_FALSE(decoded);
  EXPECT_EQ(decoded.error().code, CodecErrorCode::InvalidField);
  EXPECT_EQ(decoded.error().field_path, "project.sequences[0].tracks[0].visible");
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

TEST(ProjectCodecTest, RejectsTitlePayloadsOutsideSupportedSafetyBounds) {
  auto project = makeComplexProject();
  auto& title = *project.sequences[0].tracks[0].clips[1].title;

  title.text.assign((64U * 1024U) + 1U, 'x');
  EXPECT_THROW(static_cast<void>(serialize_project(project)), CodecException);

  title.text = "safe";
  title.font_family.assign(1025U, 'f');
  EXPECT_THROW(static_cast<void>(serialize_project(project)), CodecException);

  title.font_family = "Inter";
  title.font_size = 0.5;
  EXPECT_THROW(static_cast<void>(serialize_project(project)), CodecException);

  title.font_size = 4097.0;
  EXPECT_THROW(static_cast<void>(serialize_project(project)), CodecException);
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

} // namespace
} // namespace video_editor::project_codec
