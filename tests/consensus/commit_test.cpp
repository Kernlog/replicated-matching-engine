#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

std::uint32_t elect(Cluster& cluster) {
  EXPECT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  return cluster.leader_id();
}

std::vector<Entry> commands(const std::vector<Entry>& entries) {
  std::vector<Entry> out;
  std::ranges::copy_if(entries, std::back_inserter(out),
                       [](const Entry& entry) { return entry.type == EntryType::kCommand; });
  return out;
}

bool is_prefix(const std::vector<Entry>& a, const std::vector<Entry>& b) {
  const auto& [shorter, longer] = a.size() <= b.size() ? std::tie(a, b) : std::tie(b, a);
  return std::equal(shorter.begin(), shorter.end(), longer.begin());
}

TEST(CommitTest, SingleNodeCommitsOnPropose) {
  Cluster cluster(1);
  const std::uint32_t leader = elect(cluster);
  cluster.propose(leader, 3);
  EXPECT_EQ(cluster.node(leader).commit(), cluster.node(leader).log().last_index());
  EXPECT_EQ(commands(cluster.applied(leader)).size(), 3U);
}

TEST(CommitTest, EveryNodeAppliesTheSameEntriesInOrder) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  cluster.propose(leader, 20);
  cluster.tick_all();
  for (std::uint32_t id = 1; id <= 3; ++id) {
    EXPECT_EQ(cluster.node(id).commit(), cluster.node(leader).log().last_index()) << id;
    EXPECT_EQ(cluster.applied(id), cluster.log_of(leader)) << id;
  }
}

TEST(CommitTest, LeaderWithoutMajorityDoesNotCommit) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  cluster.propose(leader, 2);
  cluster.tick_all();
  const Index committed = cluster.node(leader).commit();
  cluster.isolate(leader);
  cluster.propose(leader, 5);
  cluster.node(leader).tick();
  cluster.deliver();
  EXPECT_EQ(cluster.node(leader).commit(), committed);
  EXPECT_EQ(cluster.applied(leader).size(), committed.value());
}

TEST(CommitTest, FiveNodesCommitWithTwoFollowersDown) {
  Cluster cluster(5);
  const std::uint32_t leader = elect(cluster);
  std::uint32_t down = 0;
  for (std::uint32_t id = 1; id <= 5 && down < 2; ++id) {
    if (id != leader) {
      cluster.isolate(id);
      ++down;
    }
  }
  cluster.propose(leader, 4);
  EXPECT_EQ(cluster.node(leader).commit(), cluster.node(leader).log().last_index());
}

TEST(CommitTest, RestartedFollowerReappliesCommittedLog) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  cluster.propose(leader, 6);
  cluster.tick_all();
  const std::uint32_t follower = leader == 1 ? 2 : 1;
  cluster.restart(follower);
  EXPECT_TRUE(cluster.applied(follower).empty());
  cluster.tick_all();
  EXPECT_EQ(cluster.applied(follower), cluster.applied(leader));
}

TEST(CommitTest, AppliedSequencesStayPrefixesAcrossLeaderChanges) {
  Cluster cluster(3, 9);
  for (int round = 0; round < 6; ++round) {
    const std::uint32_t leader = elect(cluster);
    cluster.propose(leader, 5);
    cluster.isolate(leader);
    cluster.propose(leader, 2);
    ASSERT_TRUE(cluster.run_until([&] {
      const std::uint32_t next = cluster.leader_id();
      return next != 0 && next != leader;
    }));
    cluster.heal();
    cluster.tick_all();
    for (std::uint32_t a = 1; a <= 3; ++a) {
      for (std::uint32_t b = a + 1; b <= 3; ++b) {
        EXPECT_TRUE(is_prefix(cluster.applied(a), cluster.applied(b))) << round;
      }
    }
  }
}

}  // namespace
}  // namespace rme::consensus
