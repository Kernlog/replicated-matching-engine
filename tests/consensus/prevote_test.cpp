#include <gtest/gtest.h>

#include <cstdint>
#include <variant>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

std::uint32_t elect(Cluster& cluster) {
  EXPECT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  return cluster.leader_id();
}

std::uint32_t follower_of(std::uint32_t leader) {
  return leader == 1 ? 2 : 1;
}

TEST(PreVoteTest, IsolatedNodeDoesNotBumpItsTerm) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  const Term term = cluster.node(leader).term();
  const std::uint32_t lonely = follower_of(leader);
  cluster.isolate(lonely);
  for (int tick = 0; tick < 200; ++tick) {
    cluster.tick_all();
  }
  EXPECT_EQ(cluster.node(lonely).term(), term);
  EXPECT_EQ(cluster.node(lonely).role(), Role::kPreCandidate);
}

TEST(PreVoteTest, RejoiningNodeDoesNotDeposeLeader) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  const Term term = cluster.node(leader).term();
  const std::uint32_t lonely = follower_of(leader);
  cluster.isolate(lonely);
  for (int tick = 0; tick < 200; ++tick) {
    cluster.tick_all();
  }
  cluster.heal();
  for (int tick = 0; tick < 50; ++tick) {
    cluster.tick_all();
    ASSERT_EQ(cluster.leader_id(), leader) << tick;
  }
  EXPECT_EQ(cluster.node(leader).term(), term);
  EXPECT_EQ(cluster.node(lonely).role(), Role::kFollower);
  EXPECT_EQ(cluster.node(lonely).leader(), NodeId(leader));
}

TEST(PreVoteTest, WithoutPreVoteRejoiningNodeForcesAnElection) {
  Cluster cluster(3, 1, 64, false);
  const std::uint32_t leader = elect(cluster);
  const Term term = cluster.node(leader).term();
  const std::uint32_t lonely = follower_of(leader);
  cluster.isolate(lonely);
  for (int tick = 0; tick < 200; ++tick) {
    cluster.tick_all();
  }
  cluster.heal();
  cluster.tick_all();
  EXPECT_GT(cluster.node(leader).term(), term);
}

TEST(PreVoteTest, PreCandidateChangesNoDurableState) {
  Raft raft(RaftConfig{.id = NodeId(1), .members = {NodeId(1), NodeId(2), NodeId(3)}},
            HardState{Term(4), std::nullopt});
  while (raft.role() == Role::kFollower) {
    raft.tick();
  }
  const Ready ready = raft.ready();
  EXPECT_FALSE(ready.hard_state.has_value());
  ASSERT_EQ(ready.messages.size(), 2U);
  EXPECT_EQ(ready.messages[0].term, Term(5));
  EXPECT_TRUE(std::get<VoteRequest>(ready.messages[0].body).pre_vote);
  EXPECT_EQ(raft.term(), Term(4));
}

TEST(PreVoteTest, FollowerInLeaderLeaseRejectsPreVote) {
  Raft raft(RaftConfig{.id = NodeId(1), .members = {NodeId(1), NodeId(2), NodeId(3)}});
  raft.step({NodeId(2), NodeId(1), Term(1), AppendRequest{}});
  raft.ready();
  raft.step({NodeId(3), NodeId(1), Term(2), VoteRequest{Index(5), Term(1), true}});
  const Ready ready = raft.ready();
  ASSERT_EQ(ready.messages.size(), 1U);
  EXPECT_FALSE(std::get<VoteResponse>(ready.messages[0].body).granted);
  EXPECT_EQ(ready.messages[0].term, Term(1));
  EXPECT_EQ(raft.term(), Term(1));
}

}  // namespace
}  // namespace rme::consensus
