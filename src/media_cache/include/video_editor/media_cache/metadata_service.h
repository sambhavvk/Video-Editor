// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/media_cache/cache_store.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace video_editor::media_cache {

// A persistent, per-asset editable metadata document. This is the backing
// store for the future metadata editor panel: user-editable title, tags,
// notes, rating, and custom key/value fields. It is rebuildable state —
// deleting the cache never destroys project edits or original media, but it
// does lose user-entered metadata notes.
//
// The document is distinct from the probed technical metadata in
// media::AssetDescriptor (codec, duration, etc.) which is authoritative and
// stored in the edit model. This service owns only the user-facing editorial
// metadata layer.
struct MetadataDocument {
  std::string title;        // user-editable display title; empty means "use file name"
  std::vector<std::string> tags;
  std::string notes;        // free-form notes
  int rating{0};            // 0-5, 0 means unrated
  // Custom key/value fields. Keys must be non-empty. Values are arbitrary
  // UTF-8 strings. Insertion order is preserved.
  std::vector<std::pair<std::string, std::string>> custom_fields;
};

enum class MetadataErrorCode : std::uint8_t {
  None,
  NotFound,
  InvalidArgument,
  StoreFailed,
  Internal,
};

struct MetadataError {
  MetadataErrorCode code{MetadataErrorCode::None};
  int native_code{0};
  std::string message;
};

// Result of a metadata operation. Mirrors the shape of CacheResult in
// cache_store.h: either holds a value or an error, never both.
template <typename T> class MetadataResult {
public:
  [[nodiscard]] static MetadataResult success(T value) {
    MetadataResult result;
    result.value_ = std::move(value);
    return result;
  }
  [[nodiscard]] static MetadataResult failure(MetadataError error) {
    MetadataResult result;
    result.error_ = std::move(error);
    return result;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }
  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] const T& value() const& { return value_.value(); }
  [[nodiscard]] T&& value() && { return std::move(value_).value(); }
  [[nodiscard]] const MetadataError& error() const noexcept { return error_; }

private:
  std::optional<T> value_;
  MetadataError error_;
};

// void specialization: success() takes no argument; has_value() reports
// whether the operation succeeded.
template <> class MetadataResult<void> {
public:
  [[nodiscard]] static MetadataResult success() {
    MetadataResult result;
    result.succeeded_ = true;
    return result;
  }
  [[nodiscard]] static MetadataResult failure(MetadataError error) {
    MetadataResult result;
    result.error_ = std::move(error);
    return result;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return succeeded_; }
  [[nodiscard]] bool has_value() const noexcept { return succeeded_; }
  [[nodiscard]] const MetadataError& error() const noexcept { return error_; }

private:
  bool succeeded_{false};
  MetadataError error_;
};

// The fixed parameter hash for a metadata document. There is only one
// document per asset.
[[nodiscard]] std::string metadata_parameter_hash();

// Serialize/deserialize a MetadataDocument to/from a compact little-endian
// binary blob. Magic: "VEMETA01". Layout:
//   magic(8), u16 version(=1),
//   length-prefixed UTF-8 title (u32 length + bytes),
//   u32 tag_count, then tag_count * (u32 length + bytes),
//   length-prefixed notes,
//   i32 rating,
//   u32 custom_field_count, then count * (u32 key_len + key, u32 val_len + val)
// Readers reject unknown magic, unknown version, negative rating, empty
// custom keys, duplicate custom keys, and trailing bytes.
[[nodiscard]] std::vector<std::byte> serialize_metadata(const MetadataDocument& document);
[[nodiscard]] MetadataResult<MetadataDocument> deserialize_metadata(std::span<const std::byte> bytes);

// Load the metadata document for an asset. Returns NotFound if absent (which
// the UI treats as "no user metadata — use defaults"). The cache reference is
// non-const because CacheStore::get updates the LRU access time.
[[nodiscard]] MetadataResult<MetadataDocument>
load_metadata(const std::string& asset_id, CacheStore& cache);

// Save the metadata document for an asset. Atomically replaces any existing
// document. Validates: rating in [0,5], no empty custom keys, no duplicate
// custom keys. Returns InvalidArgument on validation failure (and does NOT
// save).
[[nodiscard]] MetadataResult<void>
save_metadata(const std::string& asset_id, const MetadataDocument& document, CacheStore& cache);

// Delete the metadata document for an asset. Returns NotFound if absent.
[[nodiscard]] MetadataResult<void>
delete_metadata(const std::string& asset_id, CacheStore& cache);

// Pure validation helper. Returns true if a document is well-formed:
// rating in [0,5], no empty custom keys, no duplicate custom keys.
[[nodiscard]] bool metadata_is_valid(const MetadataDocument& document) noexcept;

} // namespace video_editor::media_cache
