#include "engine/price_ladder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace rme::engine {
namespace {

std::vector<std::uint32_t> ascending(const PriceLadder& ladder) {
  std::vector<std::uint32_t> out;
  auto collect = [&](std::uint32_t slot) { out.push_back(slot); };
  ladder.for_each_ascending(collect);
  return out;
}

std::vector<std::uint32_t> descending(const PriceLadder& ladder) {
  std::vector<std::uint32_t> out;
  auto collect = [&](std::uint32_t slot) { out.push_back(slot); };
  ladder.for_each_descending(collect);
  return out;
}

TEST(PriceLadderTest, EmptyLadderHasNoExtremes) {
  const PriceLadder ladder(8192);
  EXPECT_FALSE(ladder.lowest());
  EXPECT_FALSE(ladder.highest());
}

TEST(PriceLadderTest, TracksExtremesAcrossWordAndSummaryBoundaries) {
  PriceLadder ladder(8192);
  for (const std::uint32_t slot : {63U, 64U, 4095U, 4096U, 8191U, 0U}) {
    ladder.mark(slot);
  }
  EXPECT_EQ(ladder.lowest(), 0U);
  EXPECT_EQ(ladder.highest(), 8191U);
  ladder.clear(0);
  ladder.clear(8191);
  EXPECT_EQ(ladder.lowest(), 63U);
  EXPECT_EQ(ladder.highest(), 4096U);
  EXPECT_EQ(ascending(ladder), (std::vector<std::uint32_t>{63, 64, 4095, 4096}));
  EXPECT_EQ(descending(ladder), (std::vector<std::uint32_t>{4096, 4095, 64, 63}));
}

TEST(PriceLadderTest, ClearingOneSlotKeepsNeighboursInSameWord) {
  PriceLadder ladder(64);
  ladder.mark(5);
  ladder.mark(6);
  ladder.clear(5);
  EXPECT_EQ(ladder.lowest(), 6U);
  ladder.clear(6);
  EXPECT_FALSE(ladder.lowest());
}

}  // namespace
}  // namespace rme::engine
