// SPDX-License-Identifier: MPL-2.0

#include "video_editor/media_cache/metadata_service.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace video_editor::media_cache {
namespace {

// ---- Byte-blob builder for crafting raw blobs in tests ----

std::vector<std::byte> to_bytes(const std::string& s) {
  std::vector<std::byte> out(s.size());
  std::memcpy(out.data(), s.data(), s.size());
  return out;
}

void append_u16(std::vector<std::byte>& out, std::uint16_t v) {
  out.push_back(static_cast<std::byte>(v & 0xFFu));
  out.push_back(static_cast<std::byte>((v >> 8u) & 0xFFu));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t v) {
  out.push_back(static_cast<std::byte>(v & 0xFFu));
  out.push_back(static_cast<std::byte>((v >> 8u) & 0xFFu));
  out.push_back(static_cast<std::byte>((v >> 16u) & 0xFFu));
  out.push_back(static_cast<std::byte>((v >> 24u) & 0xFFu));
}

void append_i32(std::vector<std::byte>& out, std::int32_t v) {
  append_u32(out, static_cast<std::uint32_t>(v));
}

void append_string(std::vector<std::byte>& out, const std::string& s) {
  append_u32(out, static_cast<std::uint32_t>(s.size()));
  const auto b = to_bytes(s);
  out.insert(out.end(), b.begin(), b.end());
}

// Builds a blob from raw fields. Mirrors the serializer layout but lets tests
// inject invalid values (bad rating, duplicate keys, etc.).
std::vector<std::byte> build_blob(std::uint16_t version,
                                  const std::string& title,
                                  const std::vector<std::string>& tags,
                                  const std::string& notes,
                                  std::int32_t rating,
                                  const std::vector<std::pair<std::string, std::string>>& fields) {
  std::vector<std::byte> out;
  const auto magic = to_bytes("VEMETA01");
  out.insert(out.end(), magic.begin(), magic.end());
  append_u16(out, version);
  append_string(out, title);
  append_u32(out, static_cast<std::uint32_t>(tags.size()));
  for (const auto& t : tags) {
    append_string(out, t);
  }
  append_string(out, notes);
  append_i32(out, rating);
  append_u32(out, static_cast<std::uint32_t>(fields.size()));
  for (const auto& [k, v] : fields) {
    append_string(out, k);
    append_string(out, v);
  }
  return out;
}

MetadataDocument sample_document() {
  MetadataDocument doc;
  doc.title = "Clip One";
  doc.tags = {"intro", "b-roll", "favorite"};
  doc.notes = "shot at dawn";
  doc.rating = 4;
  doc.custom_fields = {{"location", "Berlin"}, {"camera", "Sony A7"}};
  return doc;
}

// ---- Tests ----

TEST(MetadataServiceTest, ParameterHashIsConstantV1) {
  EXPECT_EQ(metadata_parameter_hash(), "v1");
}

TEST(MetadataServiceTest, ValidationAcceptsEmptyDocument) {
  MetadataDocument doc;
  EXPECT_TRUE(metadata_is_valid(doc));
}

TEST(MetadataServiceTest, ValidationRejectsRatingOutOfRange) {
  MetadataDocument doc;
  doc.rating = -1;
  EXPECT_FALSE(metadata_is_valid(doc));
  doc.rating = 6;
  EXPECT_FALSE(metadata_is_valid(doc));
}

TEST(MetadataServiceTest, ValidationRejectsEmptyCustomKey) {
  MetadataDocument doc;
  doc.custom_fields = {{"", "value"}};
  EXPECT_FALSE(metadata_is_valid(doc));
}

TEST(MetadataServiceTest, ValidationRejectsDuplicateCustomKeys) {
  MetadataDocument doc;
  doc.custom_fields = {{"k", "v1"}, {"k", "v2"}};
  EXPECT_FALSE(metadata_is_valid(doc));
}

TEST(MetadataServiceTest, ValidationAcceptsValidDocument) {
  MetadataDocument doc = sample_document();
  EXPECT_TRUE(metadata_is_valid(doc));
}

TEST(MetadataServiceTest, SerializeDeserializeRoundTrip) {
  const MetadataDocument doc = sample_document();
  const auto bytes = serialize_metadata(doc);
  ASSERT_FALSE(bytes.empty());
  auto result = deserialize_metadata(bytes);
  ASSERT_TRUE(result.has_value()) << result.error().message;
  const auto& out = result.value();
  EXPECT_EQ(out.title, doc.title);
  EXPECT_EQ(out.tags, doc.tags);
  EXPECT_EQ(out.notes, doc.notes);
  EXPECT_EQ(out.rating, doc.rating);
  EXPECT_EQ(out.custom_fields, doc.custom_fields);
}

TEST(MetadataServiceTest, SerializePreservesCustomFieldOrder) {
  MetadataDocument doc;
  doc.custom_fields = {{"A", "1"}, {"B", "2"}};
  const auto bytes = serialize_metadata(doc);
  auto result = deserialize_metadata(bytes);
  ASSERT_TRUE(result.has_value()) << result.error().message;
  const auto& out = result.value();
  ASSERT_EQ(out.custom_fields.size(), 2u);
  EXPECT_EQ(out.custom_fields[0].first, "A");
  EXPECT_EQ(out.custom_fields[1].first, "B");
}

TEST(MetadataServiceTest, DeserializeRejectsBadMagic) {
  auto blob = build_blob(1, "", {}, "", 0, {});
  // Overwrite magic with "BADMAGIC0".
  const auto bad = to_bytes("BADMAGIC0");
  ASSERT_GE(blob.size(), bad.size());
  for (std::size_t i = 0; i < bad.size(); ++i) {
    blob[i] = bad[i];
  }
  auto result = deserialize_metadata(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, MetadataErrorCode::Internal);
}

TEST(MetadataServiceTest, DeserializeRejectsUnknownVersion) {
  auto blob = build_blob(2, "", {}, "", 0, {});
  auto result = deserialize_metadata(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, MetadataErrorCode::Internal);
}

TEST(MetadataServiceTest, DeserializeRejectsNegativeRating) {
  auto blob = build_blob(1, "", {}, "", -1, {});
  auto result = deserialize_metadata(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, MetadataErrorCode::InvalidArgument);
}

TEST(MetadataServiceTest, DeserializeRejectsEmptyCustomKey) {
  auto blob = build_blob(1, "", {}, "", 0, {{"", "v"}});
  auto result = deserialize_metadata(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, MetadataErrorCode::InvalidArgument);
}

TEST(MetadataServiceTest, DeserializeRejectsDuplicateCustomKeys) {
  auto blob = build_blob(1, "", {}, "", 0, {{"k", "v1"}, {"k", "v2"}});
  auto result = deserialize_metadata(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, MetadataErrorCode::InvalidArgument);
}

TEST(MetadataServiceTest, DeserializeRejectsTrailingBytes) {
  auto blob = build_blob(1, "", {}, "", 0, {});
  blob.push_back(static_cast<std::byte>(0x00));
  auto result = deserialize_metadata(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, MetadataErrorCode::Internal);
}

TEST(MetadataServiceTest, DeserializeRejectsTruncatedString) {
  // Build a blob whose title length prefix says 100 but only 10 bytes follow.
  std::vector<std::byte> blob;
  const auto magic = to_bytes("VEMETA01");
  blob.insert(blob.end(), magic.begin(), magic.end());
  append_u16(blob, 1);
  append_u32(blob, 100); // claims 100 bytes
  const auto ten = to_bytes("0123456789");
  blob.insert(blob.end(), ten.begin(), ten.end());
  // Nothing else — reader should fail when it tries to read 100 bytes.
  auto result = deserialize_metadata(blob);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, MetadataErrorCode::Internal);
}

} // namespace
} // namespace video_editor::media_cache
