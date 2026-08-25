// SPDX-License-Identifier: MPL-2.0
#include "video_editor/render_engine/lut3d.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace video_editor::render {
namespace {

struct LutCacheEntry final {
  std::filesystem::file_time_type mtime{};
  std::uintmax_t size{0};
  Lut3D lut;
};

std::mutex g_lut_cache_mutex;
std::unordered_map<std::string, LutCacheEntry> g_lut_cache;

[[nodiscard]] bool parse_float(std::string_view token, float& value) {
  if (token.empty()) {
    return false;
  }
  const char* begin = token.data();
  const char* end = begin + token.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end && std::isfinite(value);
}

[[nodiscard]] std::optional<std::array<float, 3>> parse_triplet(std::string_view line) {
  std::array<float, 3> values{};
  std::size_t consumed = 0;
  for (std::size_t channel = 0; channel < values.size(); ++channel) {
    const auto start = line.find_first_not_of(" \t", consumed);
    if (start == std::string_view::npos) {
      return std::nullopt;
    }
    const auto end = line.find_first_of(" \t", start);
    const auto token = line.substr(start, end == std::string_view::npos ? line.size() - start
                                                                        : end - start);
    if (!parse_float(token, values[channel])) {
      return std::nullopt;
    }
    consumed = end == std::string_view::npos ? line.size() : end;
  }
  if (line.find_first_not_of(" \t", consumed) != std::string_view::npos) {
    return std::nullopt;
  }
  return values;
}

[[nodiscard]] std::string_view trim_comment(std::string_view line) {
  const auto hash = line.find('#');
  if (hash != std::string_view::npos) {
    line = line.substr(0, hash);
  }
  const auto start = line.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) {
    return {};
  }
  const auto end = line.find_last_not_of(" \t\r\n");
  return line.substr(start, end - start + 1);
}

[[nodiscard]] std::optional<Lut3D> parse_cube_text(std::string_view text) {
  Lut3D lut;
  bool size_seen = false;
  std::size_t expected_samples = 0;
  std::istringstream stream{std::string{text}};
  std::string raw_line;
  while (std::getline(stream, raw_line)) {
    const auto line = trim_comment(raw_line);
    if (line.empty()) {
      continue;
    }
    if (line.starts_with("TITLE") || line.starts_with("DOMAIN_MIN") || line.starts_with("DOMAIN_MAX")) {
      if (line.starts_with("DOMAIN_MIN")) {
        const auto values = parse_triplet(line.substr(std::string_view{"DOMAIN_MIN"}.size()));
        if (!values) {
          return std::nullopt;
        }
        lut.domain_min = *values;
      } else if (line.starts_with("DOMAIN_MAX")) {
        const auto values = parse_triplet(line.substr(std::string_view{"DOMAIN_MAX"}.size()));
        if (!values) {
          return std::nullopt;
        }
        lut.domain_max = *values;
      }
      continue;
    }
    if (line.starts_with("LUT_3D_SIZE")) {
      const auto token = trim_comment(line.substr(std::string_view{"LUT_3D_SIZE"}.size()));
      int size = 0;
      const auto result = std::from_chars(token.data(), token.data() + token.size(), size);
      if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || size < 2 ||
          size > 256) {
        return std::nullopt;
      }
      lut.size = size;
      expected_samples = static_cast<std::size_t>(size) * static_cast<std::size_t>(size) *
                         static_cast<std::size_t>(size);
      lut.lattice.reserve(expected_samples);
      size_seen = true;
      continue;
    }
    if (!size_seen) {
      return std::nullopt;
    }
    const auto values = parse_triplet(line);
    if (!values) {
      return std::nullopt;
    }
    lut.lattice.push_back(*values);
    if (lut.lattice.size() > expected_samples) {
      return std::nullopt;
    }
  }
  if (!size_seen || lut.lattice.size() != expected_samples) {
    return std::nullopt;
  }
  for (const auto& channel : lut.domain_max) {
    if (!std::isfinite(channel)) {
      return std::nullopt;
    }
  }
  for (const auto& channel : lut.domain_min) {
    if (!std::isfinite(channel)) {
      return std::nullopt;
    }
  }
  return lut;
}

[[nodiscard]] std::array<float, 3> map_domain(const Lut3D& lut, const float red, const float green,
                                              const float blue) noexcept {
  const auto map = [&](const float value, const float minimum, const float maximum) {
    if (maximum <= minimum) {
      return 0.0F;
    }
    return std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
  };
  return {map(red, lut.domain_min[0], lut.domain_max[0]),
          map(green, lut.domain_min[1], lut.domain_max[1]),
          map(blue, lut.domain_min[2], lut.domain_max[2])};
}

[[nodiscard]] const std::array<float, 3>& lattice_at(const Lut3D& lut, const int red,
                                                     const int green, const int blue) noexcept {
  const std::size_t index =
      static_cast<std::size_t>(red) +
      (static_cast<std::size_t>(green) * static_cast<std::size_t>(lut.size)) +
      (static_cast<std::size_t>(blue) * static_cast<std::size_t>(lut.size) *
       static_cast<std::size_t>(lut.size));
  return lut.lattice[index];
}

[[nodiscard]] std::array<float, 3> lerp(const std::array<float, 3>& left,
                                        const std::array<float, 3>& right,
                                        const float factor) noexcept {
  return {left[0] + ((right[0] - left[0]) * factor), left[1] + ((right[1] - left[1]) * factor),
          left[2] + ((right[2] - left[2]) * factor)};
}

} // namespace

std::array<float, 3> Lut3D::sample(const float red, const float green,
                                   const float blue) const noexcept {
  if (size < 2 || lattice.empty()) {
    return {red, green, blue};
  }
  const auto mapped = map_domain(*this, red, green, blue);
  const float scale = static_cast<float>(size - 1);
  const float scaled_red = mapped[0] * scale;
  const float scaled_green = mapped[1] * scale;
  const float scaled_blue = mapped[2] * scale;
  const int base_red = std::clamp(static_cast<int>(std::floor(scaled_red)), 0, size - 2);
  const int base_green = std::clamp(static_cast<int>(std::floor(scaled_green)), 0, size - 2);
  const int base_blue = std::clamp(static_cast<int>(std::floor(scaled_blue)), 0, size - 2);
  const float fraction_red = scaled_red - static_cast<float>(base_red);
  const float fraction_green = scaled_green - static_cast<float>(base_green);
  const float fraction_blue = scaled_blue - static_cast<float>(base_blue);

  const auto c000 = lattice_at(*this, base_red, base_green, base_blue);
  const auto c100 = lattice_at(*this, base_red + 1, base_green, base_blue);
  const auto c010 = lattice_at(*this, base_red, base_green + 1, base_blue);
  const auto c110 = lattice_at(*this, base_red + 1, base_green + 1, base_blue);
  const auto c001 = lattice_at(*this, base_red, base_green, base_blue + 1);
  const auto c101 = lattice_at(*this, base_red + 1, base_green, base_blue + 1);
  const auto c011 = lattice_at(*this, base_red, base_green + 1, base_blue + 1);
  const auto c111 = lattice_at(*this, base_red + 1, base_green + 1, base_blue + 1);

  std::array<float, 3> result{};
  if (fraction_red >= fraction_green) {
    if (fraction_green >= fraction_blue) {
      result = lerp(lerp(lerp(c000, c100, fraction_red), c110, fraction_green), c111, fraction_blue);
    } else if (fraction_red >= fraction_blue) {
      result = lerp(lerp(lerp(c000, c100, fraction_red), c101, fraction_blue), c111, fraction_green);
    } else {
      result = lerp(lerp(lerp(c000, c001, fraction_blue), c101, fraction_red), c111, fraction_green);
    }
  } else if (fraction_green >= fraction_blue) {
    result = lerp(lerp(lerp(c000, c010, fraction_green), c110, fraction_red), c111, fraction_blue);
  } else if (fraction_red >= fraction_blue) {
    result = lerp(lerp(lerp(c000, c010, fraction_green), c011, fraction_blue), c111, fraction_red);
  } else {
    result = lerp(lerp(lerp(c000, c001, fraction_blue), c011, fraction_green), c111, fraction_red);
  }
  return result;
}

std::optional<Lut3D> parse_cube_bytes(const std::span<const std::byte> bytes) {
  std::string text(bytes.size(), '\0');
  std::transform(bytes.begin(), bytes.end(), text.begin(),
                 [](const std::byte value) { return static_cast<char>(value); });
  return parse_cube_text(text);
}

std::optional<Lut3D> parse_cube_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (text.empty()) {
    return std::nullopt;
  }
  return parse_cube_text(text);
}

const Lut3D* cached_lut_for_path(const std::filesystem::path& path) noexcept {
  if (path.empty()) {
    return nullptr;
  }
  std::error_code error;
  if (!std::filesystem::exists(path, error) || error) {
    return nullptr;
  }
  const auto mtime = std::filesystem::last_write_time(path, error);
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return nullptr;
  }
  const std::string key = path.lexically_normal().string();
  {
    std::lock_guard lock(g_lut_cache_mutex);
    const auto found = g_lut_cache.find(key);
    if (found != g_lut_cache.end() && found->second.mtime == mtime && found->second.size == size) {
      return &found->second.lut;
    }
  }
  auto parsed = parse_cube_file(path);
  if (!parsed) {
    return nullptr;
  }
  std::lock_guard lock(g_lut_cache_mutex);
  auto& entry = g_lut_cache[key];
  entry.mtime = mtime;
  entry.size = size;
  entry.lut = std::move(*parsed);
  return &entry.lut;
}

} // namespace video_editor::render
