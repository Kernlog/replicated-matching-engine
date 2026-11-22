#include "core/rng.hpp"

#include <gtest/gtest.h>

#include <array>

namespace rme {
namespace {

constexpr std::uint64_t first_output(std::uint64_t seed) {
  return Rng(seed).next();
}

static_assert(first_output(0) == 0x99ec5f36cb75f2b4U);
static_assert(first_output(42) == 0x15780b2e0c2ec716U);

TEST(RngTest, PinnedSequenceForSeed) {
  Rng rng(42);
  EXPECT_EQ(rng.next(), 0x15780b2e0c2ec716U);
  EXPECT_EQ(rng.next(), 0x6104d9866d113a7eU);
  EXPECT_EQ(rng.next(), 0xae17533239e499a1U);
}

TEST(RngTest, SameSeedSameStream) {
  Rng a(7);
  Rng b(7);
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(a.next(), b.next());
  }
}

TEST(RngTest, BetweenStaysInRangeAndHitsEveryValue) {
  Rng rng(1);
  std::array<int, 7> hits{};
  for (int i = 0; i < 70'000; ++i) {
    const std::int64_t value = rng.between(-3, 3);
    ASSERT_GE(value, -3);
    ASSERT_LE(value, 3);
    ++hits.at(static_cast<std::size_t>(value + 3));
  }
  for (const int count : hits) {
    EXPECT_NEAR(count, 10'000, 500);
  }
}

TEST(RngTest, ChanceMatchesRatio) {
  Rng rng(3);
  int yes = 0;
  for (int i = 0; i < 100'000; ++i) {
    yes += rng.chance(1, 4) ? 1 : 0;
  }
  EXPECT_NEAR(yes, 25'000, 600);
}

}  // namespace
}  // namespace rme
