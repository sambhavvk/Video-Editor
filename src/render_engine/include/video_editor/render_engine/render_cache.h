// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/entity_id.h"
#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/time.h"
#include "video_editor/render_engine/frame.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace video_editor::render {

struct RenderCacheKey {
  edit::Revision revision;
  edit::EntityId sequence_id;
  edit::Time time;
  int width{0};
  int height{0};
  std::uint64_t graph_signature{0};

  friend bool operator==(const RenderCacheKey&, const RenderCacheKey&) = default;
};

struct RenderCacheKeyHash {
  std::size_t operator()(const RenderCacheKey& key) const noexcept;
};

class RenderCache final {
public:
  explicit RenderCache(std::size_t capacity_bytes);

  [[nodiscard]] std::shared_ptr<const CpuFrame> get(const RenderCacheKey& key);
  void put(RenderCacheKey key, std::shared_ptr<const CpuFrame> frame);
  void clear_revision(edit::Revision revision);
  void clear();
  [[nodiscard]] std::size_t size_bytes() const noexcept;

private:
  struct Entry {
    std::shared_ptr<const CpuFrame> frame;
    std::size_t bytes{0};
    std::list<RenderCacheKey>::iterator lru;
  };

  void evict_to_capacity();

  std::size_t capacity_bytes_;
  std::size_t size_bytes_{0};
  mutable std::mutex mutex_;
  std::list<RenderCacheKey> lru_;
  std::unordered_map<RenderCacheKey, Entry, RenderCacheKeyHash> entries_;
};

} // namespace video_editor::render

