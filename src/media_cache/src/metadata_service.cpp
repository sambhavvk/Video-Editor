// SPDX-License-Identifier: MPL-2.0

#include "video_editor/media_cache/metadata_service.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace video_editor::media_cache {

namespace {

constexpr std::string_view kMagic = "VEMETA01";
constexpr std::uint16_t kVersion = 1;

// DoS guards. These are generous upper bounds; well-formed documents from
// real users stay far below them.
constexpr std::uint32_t kMaxStringBytes = 10ULL * 1024 * 1024;        // 10 MiB per string
constexpr std::uint32_t kMaxTagCount = 10000;
constexpr std::uint32_t kMaxCustomFieldCount = 10000;

// ---- Serialization helpers (little-endian) ----

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
  const auto len = static_cast<std::uint32_t>(s.size());
  append_u32(out, len);
  out.insert(out.end(),
             reinterpret_cast<const std::byte*>(s.data()),
             reinterpret_cast<const std::byte*>(s.data()) + s.size());
}

// ---- Deserialization helpers ----

struct ByteReader {
  std::span<const std::byte> data;
  std::size_t pos{0};

  [[nodiscard]] bool eof() const noexcept { return pos >= data.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data.size() - pos; }

  [[nodiscard]] bool read_u16(std::uint16_t& out) {
    if (remaining() < 2) {
      return false;
    }
    out = static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[pos])) |
          (static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[pos + 1])) << 8u);
    pos += 2;
    return true;
  }

  [[nodiscard]] bool read_u32(std::uint32_t& out) {
    if (remaining() < 4) {
      return false;
    }
    out = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos])) |
          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 1])) << 8u) |
          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 2])) << 16u) |
          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 3])) << 24u);
    pos += 4;
    return true;
  }

  [[nodiscard]] bool read_i32(std::int32_t& out) {
    std::uint32_t u = 0;
    if (!read_u32(u)) {
      return false;
    }
    out = static_cast<std::int32_t>(u);
    return true;
  }

  [[nodiscard]] bool read_string(std::string& out, std::uint32_t max_bytes) {
    std::uint32_t len = 0;
    if (!read_u32(len)) {
      return false;
    }
    if (len > max_bytes) {
      return false;
    }
    if (remaining() < len) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(data.data() + pos), len);
    pos += len;
    return true;
  }
};

MetadataError make_error(MetadataErrorCode code, std::string message) {
  MetadataError e;
  e.code = code;
  e.message = std::move(message);
  return e;
}

} // namespace

std::string metadata_parameter_hash() { return "v1"; }

std::vector<std::byte> serialize_metadata(const MetadataDocument& document) {
  std::vector<std::byte> out;
  out.reserve(64 + document.title.size() + document.notes.size());

  // Magic (8 bytes, ASCII).
  for (char c : kMagic) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  append_u16(out, kVersion);

  append_string(out, document.title);

  append_u32(out, static_cast<std::uint32_t>(document.tags.size()));
  for (const auto& tag : document.tags) {
    append_string(out, tag);
  }

  append_string(out, document.notes);

  append_i32(out, static_cast<std::int32_t>(document.rating));

  append_u32(out, static_cast<std::uint32_t>(document.custom_fields.size()));
  for (const auto& [key, val] : document.custom_fields) {
    append_string(out, key);
    append_string(out, val);
  }

  return out;
}

MetadataResult<MetadataDocument> deserialize_metadata(std::span<const std::byte> bytes) {
  ByteReader reader{bytes, 0};

  // Magic.
  if (reader.remaining() < kMagic.size()) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob too short for magic"));
  }
  for (std::size_t i = 0; i < kMagic.size(); ++i) {
    const auto expected = static_cast<std::byte>(static_cast<unsigned char>(kMagic[i]));
    if (reader.data[reader.pos + i] != expected) {
      return MetadataResult<MetadataDocument>::failure(
          make_error(MetadataErrorCode::Internal, "metadata blob has bad magic"));
    }
  }
  reader.pos += kMagic.size();

  // Version.
  std::uint16_t version = 0;
  if (!reader.read_u16(version)) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob truncated at version"));
  }
  if (version != kVersion) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob has unknown version"));
  }

  MetadataDocument doc;

  // Title.
  if (!reader.read_string(doc.title, kMaxStringBytes)) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob truncated at title"));
  }

  // Tags.
  std::uint32_t tag_count = 0;
  if (!reader.read_u32(tag_count)) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob truncated at tag count"));
  }
  if (tag_count > kMaxTagCount) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob tag count exceeds limit"));
  }
  doc.tags.reserve(tag_count);
  for (std::uint32_t i = 0; i < tag_count; ++i) {
    std::string tag;
    if (!reader.read_string(tag, kMaxStringBytes)) {
      return MetadataResult<MetadataDocument>::failure(
          make_error(MetadataErrorCode::Internal, "metadata blob truncated at tag"));
    }
    doc.tags.push_back(std::move(tag));
  }

  // Notes.
  if (!reader.read_string(doc.notes, kMaxStringBytes)) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob truncated at notes"));
  }

  // Rating.
  std::int32_t rating = 0;
  if (!reader.read_i32(rating)) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob truncated at rating"));
  }
  if (rating < 0 || rating > 5) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::InvalidArgument, "metadata rating out of range [0,5]"));
  }
  doc.rating = rating;

  // Custom fields.
  std::uint32_t custom_count = 0;
  if (!reader.read_u32(custom_count)) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob truncated at custom field count"));
  }
  if (custom_count > kMaxCustomFieldCount) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob custom field count exceeds limit"));
  }
  doc.custom_fields.reserve(custom_count);
  std::unordered_set<std::string> seen_keys;
  for (std::uint32_t i = 0; i < custom_count; ++i) {
    std::string key;
    if (!reader.read_string(key, kMaxStringBytes)) {
      return MetadataResult<MetadataDocument>::failure(
          make_error(MetadataErrorCode::Internal, "metadata blob truncated at custom key"));
    }
    if (key.empty()) {
      return MetadataResult<MetadataDocument>::failure(
          make_error(MetadataErrorCode::InvalidArgument, "metadata custom key is empty"));
    }
    if (!seen_keys.insert(key).second) {
      return MetadataResult<MetadataDocument>::failure(
          make_error(MetadataErrorCode::InvalidArgument, "metadata custom key duplicated"));
    }
    std::string val;
    if (!reader.read_string(val, kMaxStringBytes)) {
      return MetadataResult<MetadataDocument>::failure(
          make_error(MetadataErrorCode::Internal, "metadata blob truncated at custom value"));
    }
    doc.custom_fields.emplace_back(std::move(key), std::move(val));
  }

  // No trailing bytes allowed.
  if (!reader.eof()) {
    return MetadataResult<MetadataDocument>::failure(
        make_error(MetadataErrorCode::Internal, "metadata blob has trailing bytes"));
  }

  return MetadataResult<MetadataDocument>::success(std::move(doc));
}

bool metadata_is_valid(const MetadataDocument& document) noexcept {
  if (document.rating < 0 || document.rating > 5) {
    return false;
  }
  std::unordered_set<std::string> seen;
  for (const auto& [key, val] : document.custom_fields) {
    (void)val;
    if (key.empty()) {
      return false;
    }
    if (!seen.insert(key).second) {
      return false;
    }
  }
  return true;
}

MetadataResult<MetadataDocument>
load_metadata(const std::string& asset_id, CacheStore& cache) {
  const CacheKey key{asset_id, CacheKind::Metadata, metadata_parameter_hash()};
  auto got = cache.get(key);
  if (!got) {
    const auto& cerr = got.error();
    MetadataErrorCode code = MetadataErrorCode::StoreFailed;
    if (cerr.code == CacheErrorCode::NotFound) {
      code = MetadataErrorCode::NotFound;
    } else if (cerr.code == CacheErrorCode::InvalidArgument) {
      code = MetadataErrorCode::InvalidArgument;
    } else if (cerr.code == CacheErrorCode::Internal) {
      code = MetadataErrorCode::Internal;
    }
    return MetadataResult<MetadataDocument>::failure(
        make_error(code, "cache.get failed: " + cerr.message));
  }
  return deserialize_metadata(got.value());
}

MetadataResult<void>
save_metadata(const std::string& asset_id, const MetadataDocument& document, CacheStore& cache) {
  if (!metadata_is_valid(document)) {
    return MetadataResult<void>::failure(
        make_error(MetadataErrorCode::InvalidArgument, "metadata document failed validation"));
  }
  const CacheKey key{asset_id, CacheKind::Metadata, metadata_parameter_hash()};
  auto bytes = serialize_metadata(document);
  auto put = cache.put(key, std::span<const std::byte>{bytes});
  if (!put) {
    const auto& cerr = put.error();
    MetadataErrorCode code = MetadataErrorCode::StoreFailed;
    if (cerr.code == CacheErrorCode::InvalidArgument) {
      code = MetadataErrorCode::InvalidArgument;
    } else if (cerr.code == CacheErrorCode::Full) {
      code = MetadataErrorCode::StoreFailed;
    } else if (cerr.code == CacheErrorCode::Internal) {
      code = MetadataErrorCode::Internal;
    }
    return MetadataResult<void>::failure(
        make_error(code, "cache.put failed: " + cerr.message));
  }
  return MetadataResult<void>::success();
}

MetadataResult<void>
delete_metadata(const std::string& asset_id, CacheStore& cache) {
  const CacheKey key{asset_id, CacheKind::Metadata, metadata_parameter_hash()};
  auto rem = cache.remove(key);
  if (!rem) {
    const auto& cerr = rem.error();
    MetadataErrorCode code = MetadataErrorCode::StoreFailed;
    if (cerr.code == CacheErrorCode::NotFound) {
      code = MetadataErrorCode::NotFound;
    } else if (cerr.code == CacheErrorCode::InvalidArgument) {
      code = MetadataErrorCode::InvalidArgument;
    } else if (cerr.code == CacheErrorCode::Internal) {
      code = MetadataErrorCode::Internal;
    }
    return MetadataResult<void>::failure(
        make_error(code, "cache.remove failed: " + cerr.message));
  }
  return MetadataResult<void>::success();
}

} // namespace video_editor::media_cache
