// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace video_editor::media_cache {

// A content-addressed kind tag. The store is intentionally unaware of what a
// blob means; services interpret bytes through their own serializers.
enum class CacheKind : std::uint8_t {
  Thumbnail = 1,
  Waveform = 2,
  Metadata = 3,
};

// Options for opening a CacheStore. Defined as a freestanding struct (rather
// than a nested type) so its default member initializers are complete at the
// point of use as a default argument.
struct CacheStoreOptions {
  // Maximum total bytes the store will hold. Zero disables eviction. The
  // default mirrors the documented public-beta cache budget.
  std::uint64_t budget_bytes{100ULL * 1024 * 1024 * 1024};
  // If true, opening an existing store runs PRAGMA quick_check.
  bool integrity_check{true};
};

// Identifies one rebuildable artifact. The store keys blobs by asset id, kind,
// and a service-defined parameter hash (e.g. thumbnail width or waveform
// resolution). The parameter hash lets a service supersede an entry when its
// generation parameters change without invalidating the whole asset.
struct CacheKey {
  std::string asset_id;
  CacheKind kind{CacheKind::Thumbnail};
  std::string parameter_hash;

  [[nodiscard]] bool valid() const noexcept {
    return !asset_id.empty() && !parameter_hash.empty();
  }

  friend bool operator==(const CacheKey&, const CacheKey&) = default;
};

struct CacheInventoryEntry {
  CacheKey key;
  std::uint64_t bytes{0};
  std::int64_t last_access_utc_ms{0};
};

struct CacheInventory {
  std::uint64_t total_bytes{0};
  std::uint64_t budget_bytes{0};
  std::vector<CacheInventoryEntry> entries;
};

enum class CacheErrorCode : std::uint8_t {
  None,
  InvalidArgument,
  NotFound,
  OpenFailed,
  ReadFailed,
  WriteFailed,
  Full,
  Internal,
};

struct CacheError {
  CacheErrorCode code{CacheErrorCode::None};
  int native_code{0};
  std::string message;
};

template <typename T> class CacheResult {
public:
  [[nodiscard]] static CacheResult success(T value) {
    CacheResult result;
    result.value_ = std::move(value);
    return result;
  }
  [[nodiscard]] static CacheResult failure(CacheError error) {
    CacheResult result;
    result.error_ = std::move(error);
    return result;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }
  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_).value(); }
  [[nodiscard]] const CacheError& error() const noexcept { return error_; }

private:
  std::optional<T> value_;
  CacheError error_;
};

// void specialization: success() takes no argument; has_value() reports
// whether the operation succeeded rather than whether a value is present.
template <> class CacheResult<void> {
public:
  [[nodiscard]] static CacheResult success() {
    CacheResult result;
    result.succeeded_ = true;
    return result;
  }
  [[nodiscard]] static CacheResult failure(CacheError error) {
    CacheResult result;
    result.error_ = std::move(error);
    return result;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return succeeded_; }
  [[nodiscard]] bool has_value() const noexcept { return succeeded_; }
  [[nodiscard]] const CacheError& error() const noexcept { return error_; }

private:
  bool succeeded_{false};
  CacheError error_;
};

// A persistent, content-addressed blob store with a configurable disk budget
// and least-recently-used eviction. Backed by an on-disk SQLite index and one
// blob file per entry. The store is rebuildable: deleting its directory never
// destroys project edits or original media.
//
// The store is not thread-safe. Callers serialize access through the owning
// service. The store performs atomic writes (temp file + rename) and fsyncs
// both the blob and the index.
class CacheStore {
public:
  // Opens or creates the store at root. The directory layout is:
  //   <root>/index.sqlite
  //   <root>/blobs/<sha256-of-key>
  explicit CacheStore(std::filesystem::path root, CacheStoreOptions options = {});
  ~CacheStore();
  CacheStore(const CacheStore&) = delete;
  CacheStore& operator=(const CacheStore&) = delete;
  CacheStore(CacheStore&&) noexcept;
  CacheStore& operator=(CacheStore&&) noexcept;

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  // Stores bytes under key. Atomically replaces any existing entry. Returns
  // CacheErrorCode::Full if the blob is larger than the budget and cannot fit
  // even after evicting everything else.
  [[nodiscard]] CacheResult<void> put(const CacheKey& key, std::span<const std::byte> bytes);

  // Reads bytes under key. Updates last-access time. Returns NotFound for
  // missing entries.
  [[nodiscard]] CacheResult<std::vector<std::byte>> get(const CacheKey& key);

  // Returns true if an entry exists. Does not update last-access time.
  [[nodiscard]] CacheResult<bool> contains(const CacheKey& key);

  // Removes one entry. Returns NotFound for missing entries.
  [[nodiscard]] CacheResult<void> remove(const CacheKey& key);

  // Removes all entries for one asset across all kinds and parameter hashes.
  [[nodiscard]] CacheResult<std::uint64_t> remove_asset(const std::string& asset_id);

  // Removes all entries for one (asset, kind) across parameter hashes.
  [[nodiscard]] CacheResult<std::uint64_t> remove_kind(const std::string& asset_id, CacheKind kind);

  // Evicts least-recently-used entries until total bytes are at or below the
  // budget. Returns the number of entries evicted.
  [[nodiscard]] CacheResult<std::uint64_t> evict_to_budget();

  // Removes every entry. The store directory itself is retained.
  [[nodiscard]] CacheResult<void> clear();

  // Returns a snapshot of the store contents, sorted by last-access time
  // ascending (least-recently-used first). Useful for a future cache browser.
  [[nodiscard]] CacheResult<CacheInventory> inspect();

  [[nodiscard]] std::uint64_t budget_bytes() const noexcept { return options_.budget_bytes; }
  void set_budget_bytes(std::uint64_t bytes) noexcept { options_.budget_bytes = bytes; }

private:
  std::filesystem::path root_;
  CacheStoreOptions options_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace video_editor::media_cache
