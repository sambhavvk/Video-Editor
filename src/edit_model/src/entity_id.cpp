// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/entity_id.h"

#include <algorithm>
#include <chrono>
#include <random>

namespace video_editor::edit {
namespace {

[[nodiscard]] int hexValue(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

}  // namespace

EntityId EntityId::generate() {
  thread_local std::mt19937_64 generator([] {
    std::random_device source;
    std::seed_seq seed{source(), source(), source(), source(), source(),
                       source(), source(), source()};
    return std::mt19937_64(seed);
  }());

  std::array<std::uint8_t, 16> bytes{};
  const auto milliseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (std::size_t index = 0; index < 6; ++index) {
    bytes[index] = static_cast<std::uint8_t>(
        milliseconds >> ((5U - static_cast<unsigned>(index)) * 8U));
  }
  for (std::size_t index = 6; index < bytes.size(); index += 8) {
    const auto random = generator();
    const auto count = std::min<std::size_t>(8, bytes.size() - index);
    for (std::size_t offset = 0; offset < count; ++offset) {
      bytes[index + offset] = static_cast<std::uint8_t>(random >> (offset * 8U));
    }
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x70U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
  return EntityId(bytes);
}

std::optional<EntityId> EntityId::parse(std::string_view text) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return std::nullopt;
  }

  std::array<std::uint8_t, 16> bytes{};
  std::size_t input = 0;
  for (std::size_t output = 0; output < bytes.size(); ++output) {
    if (input == 8 || input == 13 || input == 18 || input == 23) {
      ++input;
    }
    const auto high = hexValue(text[input]);
    const auto low = hexValue(text[input + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[output] = static_cast<std::uint8_t>((high << 4) | low);
    input += 2;
  }
  return EntityId(bytes);
}

std::string EntityId::toString() const {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes_.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    result.push_back(digits[bytes_[index] >> 4U]);
    result.push_back(digits[bytes_[index] & 0x0FU]);
  }
  return result;
}

}  // namespace video_editor::edit

std::size_t std::hash<video_editor::edit::EntityId>::operator()(
    const video_editor::edit::EntityId& id) const noexcept {
  std::size_t result = 0xcbf29ce484222325ULL;
  for (const auto byte : id.bytes()) {
    result ^= byte;
    result *= 0x100000001b3ULL;
  }
  return result;
}
