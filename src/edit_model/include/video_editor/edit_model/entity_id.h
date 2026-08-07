// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace video_editor::edit {

class EntityId final {
 public:
  constexpr EntityId() noexcept = default;
  explicit constexpr EntityId(std::array<std::uint8_t, 16> bytes) noexcept
      : bytes_(bytes) {}

  [[nodiscard]] static EntityId generate();
  [[nodiscard]] static std::optional<EntityId> parse(std::string_view text);

  [[nodiscard]] constexpr bool isNil() const noexcept {
    for (const auto byte : bytes_) {
      if (byte != 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::string toString() const;
  [[nodiscard]] constexpr const std::array<std::uint8_t, 16>& bytes() const
      noexcept {
    return bytes_;
  }

  friend bool operator==(const EntityId&, const EntityId&) = default;
  friend auto operator<=>(const EntityId&, const EntityId&) = default;

 private:
  std::array<std::uint8_t, 16> bytes_{};
};

}  // namespace video_editor::edit

namespace std {
template <>
struct hash<video_editor::edit::EntityId> {
  std::size_t operator()(const video_editor::edit::EntityId& id) const noexcept;
};
}  // namespace std
