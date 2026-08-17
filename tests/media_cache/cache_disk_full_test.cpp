// SPDX-License-Identifier: MPL-2.0

#include "video_editor/media_cache/cache_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace video_editor::media_cache {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("video_editor_cache_disk_full_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

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

[[nodiscard]] CacheKey make_key(std::string asset_id, CacheKind kind, std::string parameter_hash) {
  return CacheKey{.asset_id = std::move(asset_id),
                  .kind = kind,
                  .parameter_hash = std::move(parameter_hash)};
}

void write_file_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(static_cast<bool>(output));
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  output.close();
  ASSERT_TRUE(static_cast<bool>(output));
}

[[nodiscard]] std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<char> raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  }
  return bytes;
}

} // namespace

// EditorController maps CacheErrorCode::Full to cache_disk_full_ and stops the
// remaining cache-job queue. This test covers the CacheStore seam that triggers
// that latch: oversized put_file returns Full, later oversized puts keep failing,
// inspect still works, and caller originals on disk are never adopted or deleted.
TEST(CacheDiskFull, OversizedPutFileReturnsFullAndLeavesOriginalsUntouched) {
  TemporaryDirectory dir;
  CacheStore store(dir.path() / "cache");
  store.set_budget_bytes(16);

  const auto kept_key = make_key("kept", CacheKind::Thumbnail, "w=8");
  ASSERT_TRUE(store.put(kept_key, make_bytes(1, 4)).has_value());

  const auto first_bytes = make_bytes(4, 64);
  const auto first_source = dir.path() / "original-large.bin";
  write_file_bytes(first_source, first_bytes);

  const auto second_bytes = make_bytes(9, 32);
  const auto second_source = dir.path() / "original-medium.bin";
  write_file_bytes(second_source, second_bytes);

  const auto first = store.put_file(make_key("too-big", CacheKind::Proxy, "p"), first_source);
  ASSERT_FALSE(first.has_value());
  EXPECT_EQ(first.error().code, CacheErrorCode::Full);

  const auto second = store.put_file(make_key("also-too-big", CacheKind::Proxy, "q"), second_source);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, CacheErrorCode::Full);

  const auto third = store.put(make_key("blob-too-big", CacheKind::Waveform, "r"), make_bytes(2, 32));
  ASSERT_FALSE(third.has_value());
  EXPECT_EQ(third.error().code, CacheErrorCode::Full);

  EXPECT_TRUE(std::filesystem::exists(first_source));
  EXPECT_TRUE(std::filesystem::exists(second_source));
  EXPECT_EQ(read_file_bytes(first_source), first_bytes);
  EXPECT_EQ(read_file_bytes(second_source), second_bytes);

  auto inventory = store.inspect();
  ASSERT_TRUE(inventory.has_value());
  EXPECT_EQ(inventory.value().total_bytes, 4U);
  ASSERT_EQ(inventory.value().entries.size(), 1U);
  EXPECT_EQ(inventory.value().entries.front().key.asset_id, "kept");

  auto kept = store.get(kept_key);
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(kept.value(), make_bytes(1, 4));
  EXPECT_FALSE(store.contains(make_key("too-big", CacheKind::Proxy, "p")).value());
}

} // namespace video_editor::media_cache
