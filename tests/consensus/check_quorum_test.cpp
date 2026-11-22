#include <gtest/gtest.h>

#include <cstdint>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

constexpr int kElectionTicks = 10;

std::uint32_t elect(Cluster& cluster) {
  EXPECT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  return cluster.leader_id();
}

TEST(CheckQuorumTest, IsolatedLeaderStepsDownWithinTwoElectionTimeouts) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  cluster.isolate(leader);
  for (int tick = 0; tick < 2 * kElectionTicks; ++tick) {
    cluster.tick_all();
  }
  EXPECT_NE(cluster.node(leader).role(), Role::kLeader);
  EXPECT_FALSE(cluster.node(leader).leader().has_value());
}

TEST(CheckQuorumTest, LeaderWithMajorityStaysLeader) {
  Cluster cluster(5);
  const std::uint32_t leader = elect(cluster);
  cluster.isolate(leader == 5 ? 4 : 5);
  cluster.isolate(leader == 1 ? 2 : 1);
  for (int tick = 0; tick < 500; ++tick) {
    cluster.tick_all();
    ASSERT_EQ(cluster.node(leader).role(), Role::kLeader) << tick;
  }
}

TEST(CheckQuorumTest, WithoutCheckQuorumIsolatedLeaderKeepsLeading) {
  Cluster cluster(3, {.check_quorum = false});
  const std::uint32_t leader = elect(cluster);
  cluster.isolate(leader);
  for (int tick = 0; tick < 500; ++tick) {
    cluster.tick_all();
  }
  EXPECT_EQ(cluster.node(leader).role(), Role::kLeader);
}

TEST(CheckQuorumTest, FollowerInLeaseIgnoresVoteRequest) {
  Raft raft(RaftConfig{.id = NodeId(1), .members = {NodeId(1), NodeId(2), NodeId(3)}});
  raft.step({NodeId(2), NodeId(1), Term(1), AppendRequest{}});
  raft.ready();
  raft.step({NodeId(3), NodeId(1), Term(7), VoteRequest{Index(9), Term(6), false}});
  EXPECT_EQ(raft.term(), Term(1));
  EXPECT_EQ(raft.leader(), NodeId(2));
  EXPECT_TRUE(raft.ready().messages.empty());
}

}  // namespace
}  // namespace rme::consensus
