// SPDX-License-Identifier: MPL-2.0

#include "video_editor/media_cache/cache_store.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace video_editor::media_cache {
namespace {

// ---------------------------------------------------------------------------
// TemporaryDirectory RAII helper, mirroring the pattern in
// tests/proxy_service/proxy_service_test.cpp.
// ---------------------------------------------------------------------------
class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("video_editor_media_cache_test_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::byte> make_bytes(std::uint8_t seed, std::size_t count) {
  std::vector<std::byte> out(count);
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = static_cast<std::byte>(static_cast<std::uint8_t>((seed + i) & 0xffU));
  }
  return out;
}

[[nodiscard]] CacheKey make_key(std::string asset_id, CacheKind kind,
                                std::string parameter_hash) {
  return CacheKey{.asset_id = std::move(asset_id), .kind = kind,
                 .parameter_hash = std::move(parameter_hash)};
}

} // namespace

// ---------------------------------------------------------------------------
// PutAndGetRoundTrip
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, PutAndGetRoundTrip) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());
  const auto key = make_key("asset-1", CacheKind::Thumbnail, "w=128");
  const auto bytes = make_bytes(1, 64);

  ASSERT_TRUE(store.put(key, bytes).has_value());

  auto result = store.get(key);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), bytes);
}

// ---------------------------------------------------------------------------
// MissingKeyReturnsNotFound
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, MissingKeyReturnsNotFound) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());
  const auto key = make_key("asset-missing", CacheKind::Thumbnail, "w=128");

  auto result = store.get(key);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, CacheErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// ContainsReportsPresenceWithoutMutatingAccessTime
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, ContainsReportsPresenceWithoutMutatingAccessTime) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());
  const auto key = make_key("asset-1", CacheKind::Thumbnail, "w=128");
  const auto bytes = make_bytes(1, 16);

  ASSERT_TRUE(store.put(key, bytes).has_value());

  EXPECT_TRUE(store.contains(key).value());
  EXPECT_FALSE(store.contains(make_key("absent", CacheKind::Thumbnail, "w=128")).value());
}

// ---------------------------------------------------------------------------
// RemoveDeletesEntry
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, RemoveDeletesEntry) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());
  const auto key = make_key("asset-1", CacheKind::Thumbnail, "w=128");
  const auto bytes = make_bytes(1, 16);

  ASSERT_TRUE(store.put(key, bytes).has_value());

  ASSERT_TRUE(store.remove(key).has_value());

  auto get_result = store.get(key);
  ASSERT_FALSE(get_result.has_value());
  EXPECT_EQ(get_result.error().code, CacheErrorCode::NotFound);

  auto remove_again = store.remove(key);
  ASSERT_FALSE(remove_again.has_value());
  EXPECT_EQ(remove_again.error().code, CacheErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// RemoveAssetClearsAllKindsAndParams
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, RemoveAssetClearsAllKindsAndParams) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());

  const std::string asset_id = "asset-1";
  ASSERT_TRUE(store.put(make_key(asset_id, CacheKind::Thumbnail, "w=128"), make_bytes(1, 8)).has_value());
  ASSERT_TRUE(store.put(make_key(asset_id, CacheKind::Waveform, "r=low"), make_bytes(2, 8)).has_value());
  ASSERT_TRUE(store.put(make_key(asset_id, CacheKind::Metadata, "v=1"), make_bytes(3, 8)).has_value());

  auto removed = store.remove_asset(asset_id);
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed.value(), 3U);

  EXPECT_FALSE(store.contains(make_key(asset_id, CacheKind::Thumbnail, "w=128")).value());
  EXPECT_FALSE(store.contains(make_key(asset_id, CacheKind::Waveform, "r=low")).value());
  EXPECT_FALSE(store.contains(make_key(asset_id, CacheKind::Metadata, "v=1")).value());
}

// ---------------------------------------------------------------------------
// RemoveKindClearsOnlyOneKind
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, RemoveKindClearsOnlyOneKind) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());

  const std::string asset_id = "asset-1";
  ASSERT_TRUE(store.put(make_key(asset_id, CacheKind::Thumbnail, "w=128"), make_bytes(1, 8)).has_value());
  ASSERT_TRUE(store.put(make_key(asset_id, CacheKind::Waveform, "r=low"), make_bytes(2, 8)).has_value());

  auto removed = store.remove_kind(asset_id, CacheKind::Thumbnail);
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(removed.value(), 1U);

  EXPECT_FALSE(store.contains(make_key(asset_id, CacheKind::Thumbnail, "w=128")).value());
  EXPECT_TRUE(store.contains(make_key(asset_id, CacheKind::Waveform, "r=low")).value());
}

// ---------------------------------------------------------------------------
// EvictToBudgetRemovesLeastRecentlyUsed
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, EvictToBudgetRemovesLeastRecentlyUsed) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());
  store.set_budget_bytes(10);

  const auto key_a = make_key("a", CacheKind::Thumbnail, "p");
  const auto key_b = make_key("b", CacheKind::Thumbnail, "p");
  const auto key_c = make_key("c", CacheKind::Thumbnail, "p");

  ASSERT_TRUE(store.put(key_a, make_bytes(1, 4)).has_value());
  ASSERT_TRUE(store.put(key_b, make_bytes(2, 4)).has_value());

  // Touch A so it becomes most-recent; B is now the LRU candidate.
  ASSERT_TRUE(store.get(key_a).has_value());

  // Inserting C (4 bytes) pushes total to 12 > 10; B should be evicted.
  ASSERT_TRUE(store.put(key_c, make_bytes(3, 4)).has_value());

  EXPECT_TRUE(store.contains(key_a).value());
  EXPECT_FALSE(store.contains(key_b).value());
  EXPECT_TRUE(store.contains(key_c).value());
}

TEST(CacheStoreTest, EvictToBudgetRemovesOldestWhenNoTouches) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());
  store.set_budget_bytes(10);

  const auto key_a = make_key("a", CacheKind::Thumbnail, "p");
  const auto key_b = make_key("b", CacheKind::Thumbnail, "p");
  const auto key_c = make_key("c", CacheKind::Thumbnail, "p");

  ASSERT_TRUE(store.put(key_a, make_bytes(1, 4)).has_value());
  ASSERT_TRUE(store.put(key_b, make_bytes(2, 4)).has_value());
  // No get() between puts: A is oldest, so A is evicted when C arrives.
  ASSERT_TRUE(store.put(key_c, make_bytes(3, 4)).has_value());

  EXPECT_FALSE(store.contains(key_a).value());
  EXPECT_TRUE(store.contains(key_b).value());
  EXPECT_TRUE(store.contains(key_c).value());
}

// ---------------------------------------------------------------------------
// EvictSkipsProtectedEntryDuringPut
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, EvictSkipsProtectedEntryDuringPut) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());
  store.set_budget_bytes(8);

  const auto key_a = make_key("a", CacheKind::Thumbnail, "p");
  const auto key_b = make_key("b", CacheKind::Thumbnail, "p");
  const auto key_c = make_key("c", CacheKind::Thumbnail, "p");

  ASSERT_TRUE(store.put(key_a, make_bytes(1, 4)).has_value());
  ASSERT_TRUE(store.put(key_b, make_bytes(2, 4)).has_value());

  // Total is 8 == budget. Inserting C (4 bytes) -> 12 > 8; A is LRU and must be
  // evicted. C is protected during its own put.
  ASSERT_TRUE(store.put(key_c, make_bytes(3, 4)).has_value());

  EXPECT_FALSE(store.contains(key_a).value());
  EXPECT_TRUE(store.contains(key_b).value());
  EXPECT_TRUE(store.contains(key_c).value());
}

// ---------------------------------------------------------------------------
// ClearRemovesEverything
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, ClearRemovesEverything) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());

  ASSERT_TRUE(store.put(make_key("a", CacheKind::Thumbnail, "p"), make_bytes(1, 8)).has_value());
  ASSERT_TRUE(store.put(make_key("b", CacheKind::Waveform, "p"), make_bytes(2, 8)).has_value());

  ASSERT_TRUE(store.clear().has_value());

  auto inventory = store.inspect();
  ASSERT_TRUE(inventory.has_value());
  EXPECT_EQ(inventory.value().entries.size(), 0U);
  EXPECT_EQ(inventory.value().total_bytes, 0U);
}

// ---------------------------------------------------------------------------
// InspectReportsTotals
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, InspectReportsTotals) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());

  ASSERT_TRUE(store.put(make_key("a", CacheKind::Thumbnail, "p"), make_bytes(1, 16)).has_value());
  ASSERT_TRUE(store.put(make_key("b", CacheKind::Waveform, "p"), make_bytes(2, 32)).has_value());

  auto inventory = store.inspect();
  ASSERT_TRUE(inventory.has_value());
  EXPECT_EQ(inventory.value().entries.size(), 2U);
  EXPECT_EQ(inventory.value().total_bytes, 48U);
}

// ---------------------------------------------------------------------------
// LargeBlobExceedingBudgetReturnsFull
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, LargeBlobExceedingBudgetReturnsFull) {
  TemporaryDirectory dir;
  CacheStore store(dir.path());
  store.set_budget_bytes(4);

  const auto key = make_key("big", CacheKind::Thumbnail, "p");
  auto result = store.put(key, make_bytes(1, 8));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, CacheErrorCode::Full);
}

// ---------------------------------------------------------------------------
// PersistsAcrossReopen
// ---------------------------------------------------------------------------
TEST(CacheStoreTest, PersistsAcrossReopen) {
  TemporaryDirectory dir;
  const auto key = make_key("persist", CacheKind::Thumbnail, "p");
  const auto bytes = make_bytes(7, 48);

  {
    CacheStore store(dir.path());
    ASSERT_TRUE(store.put(key, bytes).has_value());
  }

  {
    CacheStore store(dir.path());
    auto result = store.get(key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), bytes);
  }
}

} // namespace video_editor::media_cache
