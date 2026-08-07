// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/time.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace video_editor::edit {
namespace {

TEST(TimeTest, RejectsZeroTimescales) {
  EXPECT_THROW((void)Time(0, 0), std::invalid_argument);
  EXPECT_THROW((void)Rate(24, 0), std::invalid_argument);
  EXPECT_THROW((void)Rate(0, 1), std::invalid_argument);
}

TEST(TimeTest, ComparesEquivalentAndLargeRationalsExactly) {
  EXPECT_EQ(Time(1, 2), Time(50, 100));
  EXPECT_LT(Time(std::numeric_limits<std::int64_t>::max() - 1,
                 std::numeric_limits<std::uint32_t>::max()),
            Time(std::numeric_limits<std::int64_t>::max(),
                 std::numeric_limits<std::uint32_t>::max()));
  EXPECT_GT(Time(-1, 3), Time(-1, 2));
}

TEST(TimeTest, RescalesWithExplicitSignedRounding) {
  const Time positive(1, 3);
  EXPECT_EQ(positive.rescaledTo(2, RoundingMode::TowardZero), Time(0, 2));
  EXPECT_EQ(positive.rescaledTo(2, RoundingMode::Floor), Time(0, 2));
  EXPECT_EQ(positive.rescaledTo(2, RoundingMode::Ceil), Time(1, 2));
  EXPECT_EQ(positive.rescaledTo(3, RoundingMode::NearestTiesEven), Time(1, 3));

  const Time negative(-1, 3);
  EXPECT_EQ(negative.rescaledTo(2, RoundingMode::TowardZero), Time(0, 2));
  EXPECT_EQ(negative.rescaledTo(2, RoundingMode::Floor), Time(-1, 2));
  EXPECT_EQ(negative.rescaledTo(2, RoundingMode::Ceil), Time(0, 2));
  EXPECT_EQ(Time(1, 4).rescaledTo(2, RoundingMode::NearestTiesEven), Time(0, 2));
  EXPECT_EQ(Time(3, 4).rescaledTo(2, RoundingMode::NearestTiesEven), Time(2, 2));
  EXPECT_EQ(Time(-3, 4).rescaledTo(2, RoundingMode::NearestTiesAway), Time(-2, 2));
}

TEST(TimeTest, AddsAndSubtractsWithoutFloatingPoint) {
  EXPECT_EQ(Time(1, 24) + Time(1, 30), Time(3, 40));
  EXPECT_EQ(Time(5, 6) - Time(1, 4), Time(7, 12));
  EXPECT_EQ(-Time(2, 3), Time(-2, 3));
  EXPECT_EQ(Time(100, 200).normalized().toString(), "1/2s");
}

TEST(TimeTest, ReportsUnrepresentableExactArithmetic) {
  EXPECT_THROW(
      (void)(Time(1, std::numeric_limits<std::uint32_t>::max()) +
             Time(1, std::numeric_limits<std::uint32_t>::max() - 1)),
      std::overflow_error);
  EXPECT_THROW(
      (void)(Time(std::numeric_limits<std::int64_t>::max(), 1) + Time(1, 1)),
      std::overflow_error);
}

TEST(TimeRangeTest, UsesHalfOpenIntervals) {
  const TimeRange range(Time(10, 1), Time(5, 1));
  EXPECT_TRUE(range.contains(Time(10, 1)));
  EXPECT_TRUE(range.contains(Time(149, 10)));
  EXPECT_FALSE(range.contains(Time(15, 1)));
  EXPECT_TRUE(range.overlaps(TimeRange(Time(14, 1), Time(2, 1))));
  EXPECT_FALSE(range.overlaps(TimeRange(Time(15, 1), Time(2, 1))));
  EXPECT_EQ(range.intersection(TimeRange(Time(12, 1), Time(10, 1))),
            TimeRange(Time(12, 1), Time(3, 1)));
  EXPECT_THROW((void)TimeRange(Time{}, Time(-1, 1)), std::invalid_argument);
}

TEST(RateTest, ConvertsNtscFramesExactly) {
  const Rate rate(30'000, 1'001);
  EXPECT_EQ(rate.frameTime(30'000), Time(1'001, 1));
  EXPECT_EQ(rate.framesAt(Time(1'001, 1), RoundingMode::TowardZero), 30'000);
  EXPECT_EQ(Rate(60, 2), Rate(30, 1));
}

}  // namespace
}  // namespace video_editor::edit
