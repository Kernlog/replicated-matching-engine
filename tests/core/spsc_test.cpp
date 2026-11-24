#include "core/spsc.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace rme {
namespace {

TEST(SpscQueue, PopsInPushOrderAndReportsFullAndEmpty) {
  SpscQueue<int> queue(4);
  EXPECT_FALSE(queue.try_pop().has_value());
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(queue.try_push(int{i}));
  }
  EXPECT_FALSE(queue.try_push(4));
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(queue.try_pop(), std::optional(i));
  }
  EXPECT_FALSE(queue.try_pop().has_value());
}

TEST(SpscQueue, WrapsAroundManyTimes) {
  SpscQueue<std::uint64_t> queue(8);
  std::uint64_t next = 0;
  for (std::uint64_t round = 0; round < 1000; ++round) {
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(queue.try_push(round * 5 + static_cast<std::uint64_t>(i)));
    }
    for (int i = 0; i < 5; ++i) {
      ASSERT_EQ(queue.try_pop(), std::optional(next++));
    }
  }
}

TEST(SpscQueue, BatchesStopAtCapacityAndAtWhatIsQueued) {
  SpscQueue<int> queue(8);
  std::vector<int> in{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  EXPECT_EQ(queue.push_n(in), 8U);
  EXPECT_EQ(queue.push_n(in), 0U);
  std::array<int, 3> out{};
  EXPECT_EQ(queue.pop_n(out), 3U);
  EXPECT_EQ(out, (std::array{0, 1, 2}));
  std::array<int, 16> rest{};
  EXPECT_EQ(queue.pop_n(rest), 5U);
  EXPECT_EQ(rest[4], 7);
  EXPECT_EQ(queue.pop_n(rest), 0U);
}

TEST(SpscQueue, MovesOwningValues) {
  SpscQueue<std::unique_ptr<int>> queue(2);
  ASSERT_TRUE(queue.try_push(std::make_unique<int>(7)));
  const std::unique_ptr<int> popped = queue.try_pop().value_or(nullptr);
  ASSERT_NE(popped, nullptr);
  EXPECT_EQ(*popped, 7);
}

TEST(SpscQueue, TwoThreadsTransferEveryItemInOrder) {
  constexpr std::uint64_t kItems = 1'000'000;
  SpscQueue<std::uint64_t> queue(1024);
  const std::jthread producer([&queue] {
    std::array<std::uint64_t, 32> batch{};
    std::uint64_t next = 0;
    while (next < kItems) {
      if (next % 3 == 0) {
        queue.push(std::uint64_t{next++});
        continue;
      }
      const std::uint64_t count = std::min<std::uint64_t>(batch.size(), kItems - next);
      for (std::uint64_t i = 0; i < count; ++i) {
        batch[i] = next + i;
      }
      next += queue.push_n(std::span(batch).first(count));
    }
  });
  std::uint64_t expected = 0;
  std::array<std::uint64_t, 64> out{};
  while (expected < kItems) {
    queue.wait(64);
    const std::size_t count = queue.pop_n(out);
    for (std::size_t i = 0; i < count; ++i) {
      ASSERT_EQ(out[i], expected++);
    }
  }
}

}  // namespace
}  // namespace rme
