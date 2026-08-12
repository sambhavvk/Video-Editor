// SPDX-License-Identifier: MPL-2.0
#include "video_editor/job_service/job_id.h"

#include <array>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>

namespace video_editor::jobs {

std::string make_job_id() {
  thread_local std::mt19937_64 generator(std::random_device{}());
  std::array<unsigned char, 16> bytes{};
  for (std::size_t index = 0; index < bytes.size(); index += sizeof(std::uint64_t)) {
    const std::uint64_t random = generator();
    for (std::size_t offset = 0; offset < sizeof(random); ++offset) {
      bytes[index + offset] = static_cast<unsigned char>((random >> (offset * 8U)) & 0xFFU);
    }
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0FU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3FU) | 0x80U);

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) {
      output << '-';
    }
    output << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return output.str();
}

bool valid_job_id(const std::string_view value) noexcept {
  if (value.size() != 36U) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const bool separator = index == 8U || index == 13U || index == 18U || index == 23U;
    if ((separator && value[index] != '-') ||
        (!separator && std::isxdigit(static_cast<unsigned char>(value[index])) == 0)) {
      return false;
    }
  }
  return true;
}

} // namespace video_editor::jobs

