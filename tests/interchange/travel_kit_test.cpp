// SPDX-License-Identifier: MPL-2.0
#include "video_editor/interchange/travel_kit.h"
#include "video_editor/project_codec/project_codec.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace video_editor::interchange {
namespace {

TEST(TravelKitTest, WritesProjectAndManifest) {
  edit::Project project;
  project.name = "Travel";
  const auto bytes = project_codec::serialize_project(project);
  const auto temp = std::filesystem::temp_directory_path() / "ve_travel_kit_test";
  std::filesystem::remove_all(temp);
  const auto result = build_travel_kit(project, bytes, temp);
  ASSERT_TRUE(result);
  EXPECT_TRUE(std::filesystem::exists(result.value().project_path));
  EXPECT_TRUE(std::filesystem::exists(result.value().manifest_path));
  std::filesystem::remove_all(temp);
}

} // namespace
} // namespace video_editor::interchange
