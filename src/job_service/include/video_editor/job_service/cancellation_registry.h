// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace video_editor::jobs {

class CancellationRegistry {
public:
  using Token = std::shared_ptr<std::atomic_bool>;

  [[nodiscard]] Token begin(std::string job_id);
  [[nodiscard]] bool cancel(std::string_view job_id) noexcept;
  void finish(std::string_view job_id) noexcept;
  [[nodiscard]] std::size_t active_count() const noexcept;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Token> tokens_;
};

} // namespace video_editor::jobs

