#include "sim/event_queue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
#include <vector>

namespace rme::sim {
namespace {

using consensus::NodeId;

std::vector<std::uint32_t> drain(EventQueue& queue, Time end) {
  std::vector<std::uint32_t> order;
  while (const auto event = queue.pop_until(end)) {
    order.push_back(std::get<Tick>(*event).node.value());
  }
  return order;
}

TEST(EventQueueTest, PopsInTimeOrder) {
  EventQueue queue;
  queue.schedule(Time(30), Tick{NodeId(3)});
  queue.schedule(Time(10), Tick{NodeId(1)});
  queue.schedule(Time(20), Tick{NodeId(2)});
  EXPECT_EQ(drain(queue, Time(100)), (std::vector<std::uint32_t>{1, 2, 3}));
}

TEST(EventQueueTest, TiesPopInInsertionOrder) {
  EventQueue queue;
  for (const std::uint32_t id : {5U, 2U, 9U, 1U}) {
    queue.schedule(Time(7), Tick{NodeId(id)});
  }
  queue.schedule(Time(6), Tick{NodeId(4)});
  EXPECT_EQ(drain(queue, Time(7)), (std::vector<std::uint32_t>{4, 5, 2, 9, 1}));
}

TEST(EventQueueTest, PopAdvancesNowToTheEvent) {
  EventQueue queue;
  queue.schedule(Time(12), Tick{NodeId(1)});
  ASSERT_TRUE(queue.pop_until(Time(50)).has_value());
  EXPECT_EQ(queue.now(), Time(12));
  queue.after(Time(5), Tick{NodeId(2)});
  ASSERT_TRUE(queue.pop_until(Time(50)).has_value());
  EXPECT_EQ(queue.now(), Time(17));
}

TEST(EventQueueTest, LaterEventsWaitAndTimeMovesToTheEnd) {
  EventQueue queue;
  queue.schedule(Time(40), Tick{NodeId(1)});
  EXPECT_FALSE(queue.pop_until(Time(39)).has_value());
  EXPECT_EQ(queue.now(), Time(39));
  EXPECT_EQ(queue.size(), 1U);
  EXPECT_EQ(drain(queue, Time(40)), (std::vector<std::uint32_t>{1}));
  EXPECT_TRUE(queue.empty());
}

TEST(EventQueueDeathTest, SchedulingInThePastAsserts) {
  EventQueue queue;
  EXPECT_FALSE(queue.pop_until(Time(10)).has_value());
  EXPECT_DEATH(queue.schedule(Time(9), Tick{NodeId(1)}), "at >= now_");
}

}  // namespace
}  // namespace rme::sim
