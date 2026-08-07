// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/render_cache.h"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace video_editor::render {
namespace {

void hash_combine(std::size_t& seed, const std::size_t value) noexcept {
  seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

} // namespace

std::size_t RenderCacheKeyHash::operator()(const RenderCacheKey& key) const noexcept {
  std::size_t seed = std::hash<std::uint64_t>{}(key.revision.value);
  hash_combine(seed, std::hash<edit::EntityId>{}(key.sequence_id));
  hash_combine(seed, std::hash<std::int64_t>{}(key.time.value()));
  hash_combine(seed, std::hash<std::uint32_t>{}(key.time.timescale()));
  hash_combine(seed, std::hash<int>{}(key.width));
  hash_combine(seed, std::hash<int>{}(key.height));
  hash_combine(seed, std::hash<std::uint64_t>{}(key.graph_signature));
  return seed;
}

RenderCache::RenderCache(const std::size_t capacity_bytes) : capacity_bytes_(capacity_bytes) {
  if (capacity_bytes == 0U) {
    throw std::invalid_argument("render cache capacity must be non-zero");
  }
}

std::shared_ptr<const CpuFrame> RenderCache::get(const RenderCacheKey& key) {
  std::scoped_lock lock(mutex_);
  const auto iterator = entries_.find(key);
  if (iterator == entries_.end()) {
    return {};
  }
  lru_.splice(lru_.begin(), lru_, iterator->second.lru);
  return iterator->second.frame;
}

void RenderCache::put(RenderCacheKey key, std::shared_ptr<const CpuFrame> frame) {
  if (!frame) {
    return;
  }
  const std::size_t bytes = static_cast<std::size_t>(frame->width()) *
                            static_cast<std::size_t>(frame->height()) * 4U * sizeof(float);
  std::scoped_lock lock(mutex_);
  if (const auto existing = entries_.find(key); existing != entries_.end()) {
    size_bytes_ -= existing->second.bytes;
    lru_.erase(existing->second.lru);
    entries_.erase(existing);
  }
  lru_.push_front(key);
  entries_.emplace(std::move(key), Entry{.frame = std::move(frame), .bytes = bytes, .lru = lru_.begin()});
  size_bytes_ += bytes;
  evict_to_capacity();
}

void RenderCache::clear_revision(const edit::Revision revision) {
  std::scoped_lock lock(mutex_);
  for (auto iterator = entries_.begin(); iterator != entries_.end();) {
    if (iterator->first.revision == revision) {
      size_bytes_ -= iterator->second.bytes;
      lru_.erase(iterator->second.lru);
      iterator = entries_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

void RenderCache::clear() {
  std::scoped_lock lock(mutex_);
  entries_.clear();
  lru_.clear();
  size_bytes_ = 0;
}

std::size_t RenderCache::size_bytes() const noexcept {
  std::scoped_lock lock(mutex_);
  return size_bytes_;
}

void RenderCache::evict_to_capacity() {
  while (size_bytes_ > capacity_bytes_ && !lru_.empty()) {
    const RenderCacheKey& key = lru_.back();
    const auto iterator = entries_.find(key);
    if (iterator != entries_.end()) {
      size_bytes_ -= iterator->second.bytes;
      entries_.erase(iterator);
    }
    lru_.pop_back();
  }
}

} // namespace video_editor::render

