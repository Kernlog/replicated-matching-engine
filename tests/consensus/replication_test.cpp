#include <gtest/gtest.h>

#include <cstdint>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

std::uint32_t elect(Cluster& cluster) {
  EXPECT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  return cluster.leader_id();
}

std::uint32_t other(std::uint32_t leader, std::uint32_t skip = 0) {
  for (std::uint32_t id = 1;; ++id) {
    if (id != leader && id != skip) {
      return id;
    }
  }
}

TEST(ReplicationTest, FollowersReceiveProposalsInOrder) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  cluster.propose(leader, 10);
  EXPECT_EQ(cluster.node(leader).log().last_index(), Index(11));
  EXPECT_TRUE(cluster.logs_converged());
  for (std::uint32_t id = 1; id <= 3; ++id) {
    EXPECT_EQ(cluster.storage(id).entries(), cluster.log_of(leader)) << id;
  }
}

TEST(ReplicationTest, FollowerRejectsProposals) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  EXPECT_FALSE(cluster.node(other(leader)).propose({std::byte{1}}).has_value());
}

TEST(ReplicationTest, LaggingFollowerCatchesUpBeyondOneBatch) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  const std::uint32_t lagging = other(leader);
  cluster.isolate(lagging);
  cluster.propose(leader, 200);
  EXPECT_EQ(cluster.node(lagging).log().last_index(), Index(1));
  cluster.heal();
  cluster.tick_all();
  EXPECT_EQ(cluster.leader_id(), leader);
  EXPECT_TRUE(cluster.logs_converged());
}

TEST(ReplicationTest, DeposedLeaderLosesItsUnreplicatedSuffix) {
  Cluster cluster(3);
  const std::uint32_t old_leader = elect(cluster);
  cluster.propose(old_leader, 2);
  cluster.isolate(old_leader);
  cluster.propose(old_leader, 5);
  EXPECT_EQ(cluster.node(old_leader).log().last_index(), Index(8));

  ASSERT_TRUE(cluster.run_until([&] {
    const std::uint32_t leader = cluster.leader_id();
    return leader != 0 && leader != old_leader;
  }));
  const std::uint32_t new_leader = cluster.leader_id();
  cluster.propose(new_leader, 3);
  cluster.heal();
  ASSERT_TRUE(cluster.run_until([&] { return cluster.logs_converged(); }));
  EXPECT_EQ(cluster.node(old_leader).role(), Role::kFollower);
  EXPECT_EQ(cluster.log_of(old_leader), cluster.log_of(new_leader));
  EXPECT_EQ(cluster.node(new_leader).log().last_index(), Index(7));
}

}  // namespace
}  // namespace rme::consensus
