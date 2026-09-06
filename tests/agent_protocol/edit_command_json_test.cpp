// SPDX-License-Identifier: MPL-2.0
#include "video_editor/agent_protocol/edit_command_json.h"

#include <gtest/gtest.h>

#include <QJsonObject>

#include <string>
#include <vector>

namespace video_editor::agent_protocol {
namespace {

using edit::EditCommand;

[[nodiscard]] edit::EntityId makeId() { return edit::EntityId::generate(); }

void expectRoundTrip(const EditCommand& original) {
  const QJsonObject encoded = encode_command(original);
  EXPECT_EQ(encoded.value(QStringLiteral("type")).toString().toStdString(),
            edit::commandType(original));

  const auto decoded = decode_command(encoded);
  ASSERT_TRUE(decoded.hasValue()) << decoded.error().message;
  EXPECT_EQ(edit::commandType(decoded.value()), edit::commandType(original));
  EXPECT_EQ(original.coalescing_key, decoded.value().coalescing_key);
  EXPECT_EQ(encode_command(original), encode_command(decoded.value()));
}

[[nodiscard]] std::vector<EditCommand> allCommandSamples() {
  const edit::EntityId sequence_id = makeId();
  const edit::EntityId track_id = makeId();
  const edit::EntityId clip_id = makeId();
  const edit::EntityId asset_id = makeId();
  const edit::EntityId effect_id = makeId();
  const edit::EntityId marker_id = makeId();
  const edit::EntityId caption_id = makeId();
  const edit::EntityId transition_id = makeId();
  const edit::EntityId bin_id = makeId();

  edit::Clip clip;
  clip.asset_id = asset_id;

  edit::Transition transition;
  transition.outgoing_clip_id = clip_id;
  transition.incoming_clip_id = makeId();

  return {
      EditCommand{edit::AddAssetCommand{}, "coalesce-a"},
      EditCommand{edit::RemoveAssetCommand{asset_id}, {}},
      EditCommand{edit::RelinkAssetCommand{.asset_id = asset_id}, {}},
      EditCommand{edit::AddSequenceCommand{}, {}},
      EditCommand{edit::RemoveSequenceCommand{sequence_id}, {}},
      EditCommand{edit::SetSequenceFormatCommand{.sequence_id = sequence_id}, {}},
      EditCommand{edit::AddTrackCommand{.sequence_id = sequence_id}, {}},
      EditCommand{edit::RemoveTrackCommand{sequence_id, track_id}, {}},
      EditCommand{edit::RenameTrackCommand{sequence_id, track_id, "Renamed"}, {}},
      EditCommand{edit::ReorderTrackCommand{sequence_id, track_id, 2}, {}},
      EditCommand{edit::SetTrackLockedCommand{sequence_id, track_id, true}, {}},
      EditCommand{edit::SetTrackVisibilityCommand{sequence_id, track_id, false}, {}},
      EditCommand{edit::SetTrackTargetedCommand{sequence_id, track_id, false}, {}},
      EditCommand{edit::InsertClipCommand{sequence_id, track_id, clip}, {}},
      EditCommand{edit::MoveClipCommand{sequence_id, clip_id, track_id}, {}},
      EditCommand{edit::TrimClipCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::SplitClipCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::RemoveClipCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::CloseGapCommand{sequence_id, track_id}, {}},
      EditCommand{edit::RollEditCommand{sequence_id, clip_id, makeId()}, {}},
      EditCommand{edit::SlipClipCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::SlideClipCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::AddMarkerCommand{sequence_id}, {}},
      EditCommand{edit::UpdateMarkerCommand{sequence_id}, {}},
      EditCommand{edit::RemoveMarkerCommand{sequence_id, marker_id}, {}},
      EditCommand{edit::AddCaptionCommand{sequence_id}, {}},
      EditCommand{edit::UpdateCaptionCommand{sequence_id}, {}},
      EditCommand{edit::RemoveCaptionCommand{sequence_id, caption_id}, {}},
      EditCommand{edit::ApplyCaptionChangeSetCommand{.sequence_id = sequence_id}, {}},
      EditCommand{edit::ApplyTimelineCutChangeSetCommand{.sequence_id = sequence_id}, {}},
      EditCommand{edit::AddClipEffectCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::RemoveClipEffectCommand{sequence_id, clip_id, effect_id}, {}},
      EditCommand{edit::SetClipEffectParameterCommand{sequence_id, clip_id, effect_id}, {}},
      EditCommand{edit::SetClipTransformCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::SetClipBlendModeCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::SetClipAudioPropertiesCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::SetClipTitleCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::SetClipSpeedCommand{sequence_id, clip_id}, {}},
      EditCommand{edit::SetTrackAudioStateCommand{sequence_id, track_id}, {}},
      EditCommand{edit::SetTrackAudioMixCommand{sequence_id, track_id}, {}},
      EditCommand{edit::AddTrackEffectCommand{sequence_id, track_id}, {}},
      EditCommand{edit::RemoveTrackEffectCommand{sequence_id, track_id, effect_id}, {}},
      EditCommand{edit::SetTrackEffectParameterCommand{sequence_id, track_id, effect_id}, {}},
      EditCommand{edit::AddTransitionCommand{sequence_id, transition}, {}},
      EditCommand{edit::UpdateTransitionCommand{sequence_id, transition}, {}},
      EditCommand{edit::RemoveTransitionCommand{sequence_id, transition_id}, {}},
      EditCommand{edit::CreateBinCommand{}, {}},
      EditCommand{edit::RenameBinCommand{bin_id, "Folder"}, {}},
      EditCommand{edit::MoveBinCommand{.bin_id = bin_id}, {}},
      EditCommand{edit::RemoveBinCommand{bin_id}, {}},
      EditCommand{edit::SetAssetBinCommand{.asset_id = asset_id}, {}},
      EditCommand{edit::SetAssetMetadataCommand{.asset_id = asset_id}, {}},
      EditCommand{edit::SetSmartQueryCommand{bin_id}, {}},
  };
}

TEST(EditCommandJsonTest, RoundTripsAllFiftyThreeCommandTypes) {
  const std::vector<EditCommand> commands = allCommandSamples();
  EXPECT_EQ(commands.size(), 53U);
  for (const EditCommand& command : commands) {
    expectRoundTrip(command);
  }
}

TEST(EditCommandJsonTest, RoundTripsCommandBatch) {
  const std::vector<EditCommand> commands = allCommandSamples();
  const QJsonArray encoded = encode_commands(commands);
  const auto decoded = decode_commands(encoded);
  ASSERT_TRUE(decoded.hasValue()) << decoded.error().message;
  EXPECT_EQ(decoded.value().size(), commands.size());
  for (std::size_t index = 0; index < commands.size(); ++index) {
    EXPECT_EQ(encode_command(commands[index]), encode_command(decoded.value()[index]));
  }
}

TEST(EditCommandJsonTest, RoundTripsCompactBytes) {
  const EditCommand command{edit::AddAssetCommand{}, "bytes"};
  const QByteArray bytes = encode_command_bytes(command);
  const auto decoded = decode_command_bytes(bytes);
  ASSERT_TRUE(decoded.hasValue()) << decoded.error().message;
  EXPECT_EQ(encode_command(command), encode_command(decoded.value()));
}

TEST(EditCommandJsonTest, UnknownTypeFailsClosed) {
  QJsonObject object;
  object.insert(QStringLiteral("type"), QStringLiteral("not_a_real_command"));
  const auto decoded = decode_command(object);
  EXPECT_FALSE(decoded.hasValue());
  EXPECT_NE(decoded.error().message.find("unknown command type"), std::string::npos);
}

TEST(EditCommandJsonTest, OmittedClipIdGeneratesNonNilId) {
  QJsonObject object;
  object.insert(QStringLiteral("type"), QStringLiteral("insert_clip"));
  object.insert(QStringLiteral("coalescing_key"), QString());
  object.insert(QStringLiteral("sequence_id"), QString::fromStdString(makeId().toString()));
  object.insert(QStringLiteral("track_id"), QString::fromStdString(makeId().toString()));
  object.insert(QStringLiteral("mode"), QStringLiteral("reject_overlap"));

  QJsonObject clip;
  clip.insert(QStringLiteral("asset_id"), QString::fromStdString(makeId().toString()));
  clip.insert(QStringLiteral("kind"), QStringLiteral("video"));
  clip.insert(QStringLiteral("name"), QString());
  clip.insert(QStringLiteral("timeline_range"), QJsonObject{
      {QStringLiteral("start"), QJsonObject{{QStringLiteral("value"), 0}, {QStringLiteral("timescale"), 1}}},
      {QStringLiteral("duration"), QJsonObject{{QStringLiteral("value"), 0}, {QStringLiteral("timescale"), 1}}},
  });
  clip.insert(QStringLiteral("source_range"), QJsonObject{
      {QStringLiteral("start"), QJsonObject{{QStringLiteral("value"), 0}, {QStringLiteral("timescale"), 1}}},
      {QStringLiteral("duration"), QJsonObject{{QStringLiteral("value"), 0}, {QStringLiteral("timescale"), 1}}},
  });
  clip.insert(QStringLiteral("playback_rate"),
              QJsonObject{{QStringLiteral("numerator"), 1}, {QStringLiteral("denominator"), 1}});
  clip.insert(QStringLiteral("reversed"), false);
  clip.insert(QStringLiteral("transform"),
              QJsonObject{{QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.0}}},
                          {QStringLiteral("scale"), QJsonObject{{QStringLiteral("x"), 1.0}, {QStringLiteral("y"), 1.0}}},
                          {QStringLiteral("rotation_degrees"), 0.0},
                          {QStringLiteral("anchor_x"), 0.5},
                          {QStringLiteral("anchor_y"), 0.5},
                          {QStringLiteral("crop_left"), 0.0},
                          {QStringLiteral("crop_top"), 0.0},
                          {QStringLiteral("crop_right"), 0.0},
                          {QStringLiteral("crop_bottom"), 0.0},
                          {QStringLiteral("opacity"), 1.0}});
  clip.insert(QStringLiteral("blend_mode"), QStringLiteral("normal"));
  clip.insert(QStringLiteral("audio_gain_db"), 0.0);
  clip.insert(QStringLiteral("audio_pan"), 0.0);
  clip.insert(QStringLiteral("fade_in"),
              QJsonObject{{QStringLiteral("value"), 0}, {QStringLiteral("timescale"), 1}});
  clip.insert(QStringLiteral("fade_out"),
              QJsonObject{{QStringLiteral("value"), 0}, {QStringLiteral("timescale"), 1}});
  clip.insert(QStringLiteral("effects"), QJsonArray());
  object.insert(QStringLiteral("clip"), clip);

  const auto decoded = decode_command(object);
  ASSERT_TRUE(decoded.hasValue()) << decoded.error().message;
  const auto* insert = std::get_if<edit::InsertClipCommand>(&decoded.value().operation);
  ASSERT_NE(insert, nullptr);
  EXPECT_FALSE(insert->clip.id.isNil());
}

}  // namespace
}  // namespace video_editor::agent_protocol
