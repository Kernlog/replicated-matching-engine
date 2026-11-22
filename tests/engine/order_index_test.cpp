#include "engine/order_index.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <map>

#include "core/rng.hpp"

namespace rme::engine {
namespace {

TEST(OrderIndexTest, InsertFindErase) {
  OrderIndex index(4);
  EXPECT_TRUE(index.insert(OrderId(7), 3));
  EXPECT_EQ(index.find(OrderId(7)), 3U);
  EXPECT_EQ(index.find(OrderId(8)), kNil);
  index.erase(OrderId(7));
  EXPECT_EQ(index.find(OrderId(7)), kNil);
  EXPECT_EQ(index.size(), 0U);
}

TEST(OrderIndexTest, DuplicateInsertKeepsFirstValue) {
  OrderIndex index(4);
  EXPECT_TRUE(index.insert(OrderId(1), 10));
  EXPECT_FALSE(index.insert(OrderId(1), 20));
  EXPECT_EQ(index.find(OrderId(1)), 10U);
  EXPECT_EQ(index.size(), 1U);
}

TEST(OrderIndexTest, EraseOfMissingKeyIsNoop) {
  OrderIndex index(4);
  index.insert(OrderId(1), 1);
  index.erase(OrderId(2));
  EXPECT_EQ(index.size(), 1U);
}

TEST(OrderIndexTest, MatchesStdMapUnderCollisionsAndWraparound) {
  constexpr std::uint32_t kMaxLive = 8;
  OrderIndex index(kMaxLive);
  std::map<std::uint64_t, std::uint32_t> reference;
  Rng rng(5);
  for (std::uint32_t step = 0; step < 200'000; ++step) {
    const OrderId key(rng.below(40));
    if (reference.size() < kMaxLive && rng.chance(1, 2)) {
      EXPECT_EQ(index.insert(key, step), reference.emplace(key.value(), step).second);
    } else if (!reference.empty()) {
      auto victim = reference.begin();
      std::advance(victim, static_cast<std::ptrdiff_t>(rng.below(reference.size())));
      index.erase(OrderId(victim->first));
      reference.erase(victim);
    }
    const auto expected = reference.find(key.value());
    ASSERT_EQ(index.find(key), expected == reference.end() ? kNil : expected->second) << step;
    ASSERT_EQ(index.size(), reference.size()) << step;
  }
  for (const auto& [key, value] : reference) {
    EXPECT_EQ(index.find(OrderId(key)), value);
  }
}

}  // namespace
}  // namespace rme::engine
