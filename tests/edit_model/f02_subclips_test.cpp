// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/edit_model.h"

#include <gtest/gtest.h>

namespace video_editor::edit {
namespace {

[[nodiscard]] Project makeProjectWithAsset() {
  Project project;
  Asset asset;
  asset.name = "interview.mov";
  asset.source_uri = "memory://interview";
  asset.fingerprint = "fp1";
  asset.duration = Time(120, 1);
  asset.has_video = true;
  asset.has_audio = true;
  asset.width = 1920;
  asset.height = 1080;
  project.assets.push_back(asset);
  return project;
}

[[nodiscard]] bool applyOk(TimelineEditor& editor, EditCommand command) {
  return static_cast<bool>(editor.apply(std::move(command), editor.revision()));
}

TEST(F02SubclipsTest, SubclipPreservesSourceIdentityAndSelectsAssemblyIsUndoable) {
  auto project = makeProjectWithAsset();
  project.assets.front().width = 3840;
  project.assets.front().height = 2160;
  project.assets.front().nominal_frame_rate = Rate(24, 1);
  const auto asset_id = project.assets.front().id;
  TimelineEditor editor(std::move(project));

  Subclip subclip;
  subclip.source_asset_id = asset_id;
  subclip.source_range = TimeRange{Time(10, 1), Time(20, 1)};
  subclip.name = "Good take";
  subclip.notes = "steady eyeline";
  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = CreateSubclipCommand{.subclip = subclip}}));

  const auto* stored = findSubclip(*editor.projectAt(editor.revision()), subclip.id);
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(stored->source_asset_id, asset_id);
  EXPECT_EQ(stored->source_range.duration, Time(20, 1));

  Subclip invalid;
  invalid.source_asset_id = asset_id;
  invalid.source_range = TimeRange{Time(-1, 1), Time(2, 1)};
  EXPECT_FALSE(applyOk(editor, EditCommand{.operation = CreateSubclipCommand{.subclip = invalid}}));

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = AssembleSelectsSequenceCommand{
                                              .sequence_name = "Selects",
                                              .subclip_ids = {subclip.id}}}));
  const auto assembled = editor.projectAt(editor.revision());
  ASSERT_EQ(assembled->sequences.size(), 1U);
  EXPECT_EQ(assembled->sequences.front().width, 3840U);
  EXPECT_EQ(assembled->sequences.front().height, 2160U);
  EXPECT_EQ(assembled->sequences.front().frame_rate, Rate(24, 1));
  ASSERT_EQ(assembled->sequences.front().tracks.size(), 2U);
  ASSERT_FALSE(assembled->sequences.front().tracks.front().clips.empty());
  ASSERT_FALSE(assembled->sequences.front().tracks.back().clips.empty());
  EXPECT_EQ(assembled->sequences.front().tracks.front().clips.front().linked_group,
            assembled->sequences.front().tracks.back().clips.front().linked_group);
  EXPECT_TRUE(assembled->sequences.front().tracks.front().clips.front().linked_group.has_value());

  EXPECT_FALSE(applyOk(editor, EditCommand{.operation = RemoveAssetCommand{.asset_id = asset_id}}));

  ASSERT_TRUE(applyOk(editor, EditCommand{.operation = RemoveSubclipCommand{.subclip_id = subclip.id}}));
  EXPECT_EQ(editor.projectAt(editor.revision())->subclips.size(), 0U);
  EXPECT_EQ(findAsset(*editor.projectAt(editor.revision()), asset_id)->fingerprint, "fp1");

  ASSERT_TRUE(editor.undo(editor.revision()));
  EXPECT_EQ(editor.projectAt(editor.revision())->subclips.size(), 1U);
}

} // namespace
} // namespace video_editor::edit
