// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/entity_id.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace video_editor::edit {
namespace {

TEST(EntityIdTest, GeneratesUuidV7AndRoundTripsCanonicalText) {
  const auto id = EntityId::generate();
  ASSERT_FALSE(id.isNil());
  EXPECT_EQ(id.bytes()[6] >> 4U, 7U);
  EXPECT_EQ(id.bytes()[8] >> 6U, 2U);

  const auto text = id.toString();
  ASSERT_EQ(text.size(), 36U);
  const auto parsed = EntityId::parse(text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, id);
}

TEST(EntityIdTest, RejectsMalformedTextAndSupportsHashing) {
  EXPECT_FALSE(EntityId::parse("not-an-id"));
  EXPECT_FALSE(EntityId::parse("00000000-0000-0000-0000-00000000000z"));

  std::unordered_set<EntityId> ids;
  for (int index = 0; index < 128; ++index) {
    ids.insert(EntityId::generate());
  }
  EXPECT_EQ(ids.size(), 128U);
}

}  // namespace
}  // namespace video_editor::edit
