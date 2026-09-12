// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

TEST(F03AudioChannelsTest, MonitoringSelectionValidatesChannelIndices) {
  Project project;
  Asset asset;
  asset.name = "boom.wav";
  asset.source_uri = "memory://boom";
  asset.duration = Time(10, 1);
  asset.has_audio = true;
  asset.audio_channels = 4;
  asset.audio_channel_map = {{0, "Ch 1"}, {1, "Ch 2"}, {2, "BOOM"}, {3, "RF"}};
  project.assets.push_back(asset);
  TimelineEditor editor(std::move(project));
  const auto asset_id = editor.projectAt(editor.revision())->assets.front().id;

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = SetAssetAudioMonitoringCommand{
                                              .asset_id = asset_id,
                                              .left_channel = 2,
                                              .right_channel = 3}}));
  const auto* stored = findAsset(*editor.projectAt(editor.revision()), asset_id);
  EXPECT_EQ(stored->monitor_left_channel, 2U);
  EXPECT_EQ(stored->monitor_right_channel, 3U);
  EXPECT_EQ(stored->audio_channel_map.size(), 4U);

  ASSERT_FALSE(applyOk(editor, EditCommand{.operation = SetAssetAudioMonitoringCommand{
                                               .asset_id = asset_id,
                                               .left_channel = 4,
                                               .right_channel = 0}}));
}

} // namespace
} // namespace video_editor::edit
