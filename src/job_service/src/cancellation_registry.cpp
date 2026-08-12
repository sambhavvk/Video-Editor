// SPDX-License-Identifier: MPL-2.0
#include "video_editor/job_service/cancellation_registry.h"

namespace video_editor::jobs {

CancellationRegistry::Token CancellationRegistry::begin(std::string job_id) {
  auto token = std::make_shared<std::atomic_bool>(false);
  std::scoped_lock lock(mutex_);
  tokens_.insert_or_assign(std::move(job_id), token);
  return token;
}

bool CancellationRegistry::cancel(const std::string_view job_id) noexcept {
  std::scoped_lock lock(mutex_);
  const auto iterator = tokens_.find(std::string(job_id));
  if (iterator == tokens_.end()) {
    return false;
  }
  iterator->second->store(true, std::memory_order_release);
  return true;
}

void CancellationRegistry::finish(const std::string_view job_id) noexcept {
  std::scoped_lock lock(mutex_);
  tokens_.erase(std::string(job_id));
}

std::size_t CancellationRegistry::active_count() const noexcept {
  std::scoped_lock lock(mutex_);
  return tokens_.size();
}

} // namespace video_editor::jobs

