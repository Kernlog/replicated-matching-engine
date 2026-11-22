#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

struct Replication {
  int rounds = 0;
  std::size_t entries = 0;
};

Replication replicate(std::uint32_t max_inflight, int entries) {
  Cluster cluster(3, {.max_entries = 16, .max_inflight = max_inflight});
  EXPECT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const std::uint32_t leader = cluster.leader_id();
  cluster.tick_all();
  const std::size_t entries_before = cluster.entries_sent();
  for (int i = 0; i < entries; ++i) {
    const std::array payload{static_cast<std::byte>(i)};
    cluster.node(leader).propose(payload);
  }
  Replication run;
  while (!cluster.logs_converged() && run.rounds < 10'000) {
    cluster.deliver_round();
    ++run.rounds;
  }
  EXPECT_TRUE(cluster.logs_converged());
  run.entries = cluster.entries_sent() - entries_before;
  return run;
}

TEST(PipelineTest, WindowCutsRoundTripsToReplicate) {
  const Replication single = replicate(1, 2048);
  const Replication pipelined = replicate(8, 2048);
  EXPECT_LT(pipelined.rounds * 4, single.rounds)
      << "window 1: " << single.rounds << " rounds, window 8: " << pipelined.rounds;
  RecordProperty("rounds_window_1", single.rounds);
  RecordProperty("rounds_window_8", pipelined.rounds);
}

TEST(PipelineTest, EntriesAreSentOncePerFollowerWithoutLoss) {
  const Replication run = replicate(8, 2048);
  EXPECT_EQ(run.entries, 2U * 2048U);
  RecordProperty("entries_sent", static_cast<int>(run.entries));
}

TEST(PipelineTest, LostAppendsAreRecoveredByProbing) {
  Cluster cluster(3, {.max_entries = 8});
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const std::uint32_t leader = cluster.leader_id();
  cluster.drop_every(5);
  cluster.propose(leader, 300);
  cluster.drop_every(0);
  ASSERT_TRUE(cluster.run_until([&] { return cluster.logs_converged(); }, 200));
  cluster.tick_all();
  EXPECT_EQ(cluster.node(leader).commit(), cluster.node(leader).log().last_index());
}

}  // namespace
}  // namespace rme::consensus
