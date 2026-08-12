// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/time.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>

#if defined(_MSC_VER) && !defined(__clang__)
#error "Exact timeline arithmetic requires clang-cl on Windows; native MSVC has no __int128 support"
#endif

namespace video_editor::edit {
namespace {

#if defined(_MSC_VER) && defined(__clang__)
using SignedWide = __int128;
using UnsignedWide = unsigned __int128;
#else
using SignedWide = __int128_t;
using UnsignedWide = __uint128_t;
#endif

[[nodiscard]] UnsignedWide absolute(SignedWide value) noexcept {
  if (value >= 0) {
    return static_cast<UnsignedWide>(value);
  }
  return static_cast<UnsignedWide>(-(value + 1)) + 1;
}

[[nodiscard]] UnsignedWide gcdWide(UnsignedWide lhs, UnsignedWide rhs) noexcept {
  while (rhs != 0) {
    const auto remainder = lhs % rhs;
    lhs = rhs;
    rhs = remainder;
  }
  return lhs;
}

[[nodiscard]] std::int64_t narrow(SignedWide value) {
  if (value < static_cast<SignedWide>(std::numeric_limits<std::int64_t>::min()) ||
      value > static_cast<SignedWide>(std::numeric_limits<std::int64_t>::max())) {
    throw std::overflow_error("time value exceeds int64 range");
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] SignedWide divideRounded(SignedWide numerator, UnsignedWide denominator,
                                       RoundingMode rounding) {
  if (denominator == 0 ||
      denominator > static_cast<UnsignedWide>(std::numeric_limits<SignedWide>::max())) {
    throw std::invalid_argument("invalid time division denominator");
  }
  const auto signed_denominator = static_cast<SignedWide>(denominator);
  auto quotient = numerator / signed_denominator;
  const auto remainder = numerator % signed_denominator;
  if (remainder == 0) {
    return quotient;
  }

  const auto sign = numerator < 0 ? SignedWide{-1} : SignedWide{1};
  switch (rounding) {
  case RoundingMode::TowardZero:
    return quotient;
  case RoundingMode::Floor:
    return numerator < 0 ? quotient - 1 : quotient;
  case RoundingMode::Ceil:
    return numerator > 0 ? quotient + 1 : quotient;
  case RoundingMode::NearestTiesAway: {
    const auto magnitude = absolute(remainder);
    return magnitude * 2 >= denominator ? quotient + sign : quotient;
  }
  case RoundingMode::NearestTiesEven: {
    const auto magnitude = absolute(remainder);
    const auto twice = magnitude * 2;
    if (twice > denominator || (twice == denominator && absolute(quotient) % 2 != 0)) {
      quotient += sign;
    }
    return quotient;
  }
  }
  throw std::invalid_argument("unknown rounding mode");
}

[[nodiscard]] Time fromRatio(SignedWide numerator, UnsignedWide denominator) {
  if (denominator == 0) {
    throw std::invalid_argument("time timescale must be non-zero");
  }
  if (numerator == 0) {
    return Time{};
  }
  const auto divisor = gcdWide(absolute(numerator), denominator);
  numerator /= static_cast<SignedWide>(divisor);
  denominator /= divisor;
  if (denominator > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("exact time timescale exceeds uint32 range");
  }
  return Time(narrow(numerator), static_cast<std::uint32_t>(denominator));
}

} // namespace

Time::Time(std::int64_t value, std::uint32_t timescale) : value_(value), timescale_(timescale) {
  if (timescale == 0) {
    throw std::invalid_argument("time timescale must be non-zero");
  }
}

Time Time::normalized() const {
  return fromRatio(static_cast<SignedWide>(value_), static_cast<UnsignedWide>(timescale_));
}

Time Time::rescaledTo(std::uint32_t target_timescale, RoundingMode rounding) const {
  if (target_timescale == 0) {
    throw std::invalid_argument("target timescale must be non-zero");
  }
  const auto numerator = static_cast<SignedWide>(value_) * target_timescale;
  const auto rounded = divideRounded(numerator, timescale_, rounding);
  return Time(narrow(rounded), target_timescale);
}

Time Time::scaled(std::int64_t numerator, std::uint32_t denominator, RoundingMode rounding) const {
  if (denominator == 0) {
    throw std::invalid_argument("scale denominator must be non-zero");
  }
  const auto product = static_cast<SignedWide>(value_) * numerator;
  const auto rounded = divideRounded(product, denominator, rounding);
  return Time(narrow(rounded), timescale_);
}

std::string Time::toString() const {
  const auto reduced = normalized();
  return std::to_string(reduced.value()) + "/" + std::to_string(reduced.timescale()) + "s";
}

bool operator==(const Time& lhs, const Time& rhs) noexcept {
  return static_cast<SignedWide>(lhs.value_) * rhs.timescale_ ==
         static_cast<SignedWide>(rhs.value_) * lhs.timescale_;
}

std::strong_ordering operator<=>(const Time& lhs, const Time& rhs) noexcept {
  const auto left = static_cast<SignedWide>(lhs.value_) * rhs.timescale_;
  const auto right = static_cast<SignedWide>(rhs.value_) * lhs.timescale_;
  if (left < right) {
    return std::strong_ordering::less;
  }
  if (left > right) {
    return std::strong_ordering::greater;
  }
  return std::strong_ordering::equal;
}

Time operator+(const Time& lhs, const Time& rhs) {
  const auto numerator = static_cast<SignedWide>(lhs.value_) * rhs.timescale_ +
                         static_cast<SignedWide>(rhs.value_) * lhs.timescale_;
  const auto denominator =
      static_cast<UnsignedWide>(lhs.timescale_) * static_cast<UnsignedWide>(rhs.timescale_);
  return fromRatio(numerator, denominator);
}

Time operator-(const Time& lhs, const Time& rhs) {
  const auto numerator = static_cast<SignedWide>(lhs.value_) * rhs.timescale_ -
                         static_cast<SignedWide>(rhs.value_) * lhs.timescale_;
  const auto denominator =
      static_cast<UnsignedWide>(lhs.timescale_) * static_cast<UnsignedWide>(rhs.timescale_);
  return fromRatio(numerator, denominator);
}

Time operator-(const Time& time) {
  if (time.value_ == std::numeric_limits<std::int64_t>::min()) {
    return fromRatio(-static_cast<SignedWide>(time.value_), time.timescale_);
  }
  return Time(-time.value_, time.timescale_);
}

TimeRange::TimeRange(Time start_time, Time range_duration)
    : start(start_time), duration(range_duration) {
  if (duration.isNegative()) {
    throw std::invalid_argument("time range duration cannot be negative");
  }
}

Time TimeRange::end() const {
  return start + duration;
}

bool TimeRange::contains(Time time) const {
  return !empty() && time >= start && time < end();
}

bool TimeRange::contains(const TimeRange& other) const {
  return other.start >= start && other.end() <= end();
}

bool TimeRange::overlaps(const TimeRange& other) const {
  return !empty() && !other.empty() && start < other.end() && other.start < end();
}

TimeRange TimeRange::intersection(const TimeRange& other) const {
  const auto intersection_start = std::max(start, other.start);
  const auto intersection_end = std::min(end(), other.end());
  if (intersection_end <= intersection_start) {
    return TimeRange(intersection_start, Time{});
  }
  return TimeRange(intersection_start, intersection_end - intersection_start);
}

Rate::Rate(std::uint32_t numerator, std::uint32_t denominator) {
  if (numerator == 0 || denominator == 0) {
    throw std::invalid_argument("rate components must be non-zero");
  }
  const auto divisor = std::gcd(numerator, denominator);
  numerator_ = numerator / divisor;
  denominator_ = denominator / divisor;
}

Time Rate::frameTime(std::int64_t frame_count) const {
  return Time(frame_count, numerator_)
      .scaled(denominator_, 1, RoundingMode::TowardZero)
      .normalized();
}

std::int64_t Rate::framesAt(Time time, RoundingMode rounding) const {
  const auto numerator = static_cast<SignedWide>(time.value()) * numerator_;
  const auto denominator =
      static_cast<UnsignedWide>(time.timescale()) * static_cast<UnsignedWide>(denominator_);
  return narrow(divideRounded(numerator, denominator, rounding));
}

} // namespace video_editor::edit
