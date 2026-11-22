#include "sim/clock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include "consensus/raft.hpp"
#include "sim/event_queue.hpp"

namespace rme::sim {
namespace {

using consensus::NodeId;

TEST(ClockTest, TicksEachNodeOncePerInterval) {
  EventQueue queue;
  Clock clock(queue, Time(10));
  clock.start(NodeId(1), Time(1));
  clock.start(NodeId(2), Time(4));
  std::vector<std::pair<std::uint64_t, std::uint32_t>> ticks;
  while (const auto event = queue.pop_until(Time(100))) {
    const Tick tick = std::get<Tick>(*event);
    ticks.emplace_back(clock.now().value(), tick.node.value());
    clock.ticked(tick);
  }
  std::vector<std::pair<std::uint64_t, std::uint32_t>> expected;
  for (std::uint64_t t = 0; t < 100; t += 10) {
    expected.emplace_back(t + 1, 1);
    expected.emplace_back(t + 4, 2);
  }
  EXPECT_EQ(ticks, expected);
  EXPECT_EQ(clock.now(), Time(100));
}

TEST(ClockTest, DrivesRaftElectionTimeout) {
  constexpr std::uint32_t kElectionTicks = 10;
  consensus::Raft raft(consensus::RaftConfig{
      .id = NodeId(1), .members = {NodeId(1)}, .election_ticks = kElectionTicks});
  EventQueue queue;
  Clock clock(queue, Time(5));
  clock.start(NodeId(1), Time(5));
  std::uint64_t ticks = 0;
  while (const auto event = queue.pop_until(Time(1000))) {
    if (raft.role() == consensus::Role::kLeader) {
      break;
    }
    raft.tick();
    ++ticks;
    clock.ticked(std::get<Tick>(*event));
  }
  ASSERT_EQ(raft.role(), consensus::Role::kLeader);
  EXPECT_GE(ticks, kElectionTicks);
  EXPECT_LT(ticks, 2 * kElectionTicks);
  EXPECT_EQ(clock.now(), Time(5 * (ticks + 1)));
}

TEST(ClockDeathTest, ZeroIntervalAsserts) {
  EventQueue queue;
  EXPECT_DEATH(Clock(queue, Time(0)), "interval > Time\\(0\\)");
}

}  // namespace
}  // namespace rme::sim
