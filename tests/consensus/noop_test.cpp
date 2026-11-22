#include <gtest/gtest.h>

#include <cstdint>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

TEST(NoopTest, NewLeaderAppendsNoopInItsTerm) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const Raft& leader = cluster.node(cluster.leader_id());
  const Entry& last = leader.log().at(leader.log().last_index());
  EXPECT_EQ(last.type, EntryType::kNoop);
  EXPECT_EQ(last.term, leader.term());
  EXPECT_TRUE(last.data.empty());
}

TEST(NoopTest, NoopCommitsEntriesFromEarlierTerms) {
  Cluster cluster(3);
  cluster.load(1, 1, {1, 1, 1});
  cluster.load(2, 1, {1, 1, 1});
  cluster.load(3, 1, {1});
  cluster.campaign(1);
  ASSERT_EQ(cluster.leader_id(), 1U);
  cluster.tick_all();
  for (std::uint32_t id = 1; id <= 3; ++id) {
    EXPECT_EQ(cluster.node(id).commit(), Index(4)) << id;
    ASSERT_EQ(cluster.applied(id).size(), 4U) << id;
    EXPECT_EQ(cluster.applied(id).back().type, EntryType::kNoop) << id;
  }
}

}  // namespace
}  // namespace rme::consensus
