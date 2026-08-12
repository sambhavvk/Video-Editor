// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace video_editor::edit {

enum class RoundingMode {
  TowardZero,
  Floor,
  Ceil,
  NearestTiesAway,
  NearestTiesEven,
};

class Time final {
 public:
  constexpr Time() noexcept = default;
  Time(std::int64_t value, std::uint32_t timescale);

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr std::uint32_t timescale() const noexcept {
    return timescale_;
  }
  [[nodiscard]] constexpr bool isNegative() const noexcept { return value_ < 0; }
  [[nodiscard]] constexpr bool isZero() const noexcept { return value_ == 0; }

  [[nodiscard]] Time normalized() const;
  [[nodiscard]] Time rescaledTo(std::uint32_t target_timescale,
                                RoundingMode rounding) const;
  [[nodiscard]] Time scaled(std::int64_t numerator,
                            std::uint32_t denominator,
                            RoundingMode rounding) const;
  [[nodiscard]] std::string toString() const;

  friend bool operator==(const Time& lhs, const Time& rhs) noexcept;
  friend std::strong_ordering operator<=>(const Time& lhs,
                                           const Time& rhs) noexcept;
  friend Time operator+(const Time& lhs, const Time& rhs);
  friend Time operator-(const Time& lhs, const Time& rhs);
  friend Time operator-(const Time& time);

 private:
  std::int64_t value_{0};
  std::uint32_t timescale_{1};
};

struct TimeRange final {
  Time start{};
  Time duration{};

  TimeRange() = default;
  TimeRange(Time start_time, Time range_duration);

  [[nodiscard]] Time end() const;
  [[nodiscard]] bool empty() const noexcept { return duration.isZero(); }
  [[nodiscard]] bool contains(Time time) const;
  [[nodiscard]] bool contains(const TimeRange& other) const;
  [[nodiscard]] bool overlaps(const TimeRange& other) const;
  [[nodiscard]] TimeRange intersection(const TimeRange& other) const;

  friend bool operator==(const TimeRange&, const TimeRange&) = default;
};

class Rate final {
 public:
  constexpr Rate() noexcept = default;
  Rate(std::uint32_t numerator, std::uint32_t denominator);

  [[nodiscard]] constexpr std::uint32_t numerator() const noexcept {
    return numerator_;
  }
  [[nodiscard]] constexpr std::uint32_t denominator() const noexcept {
    return denominator_;
  }
  [[nodiscard]] Time frameTime(std::int64_t frame_count = 1) const;
  [[nodiscard]] std::int64_t framesAt(Time time, RoundingMode rounding) const;

  friend bool operator==(const Rate&, const Rate&) = default;

 private:
  std::uint32_t numerator_{30};
  std::uint32_t denominator_{1};
};

}  // namespace video_editor::edit
