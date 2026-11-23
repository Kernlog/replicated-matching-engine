#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
#include <vector>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

RaftConfig wiped(std::uint32_t id, std::uint32_t members) {
  std::vector<NodeId> ids;
  for (std::uint32_t i = 1; i <= members; ++i) {
    ids.emplace_back(i);
  }
  return RaftConfig{.id = NodeId(id), .members = std::move(ids), .lost_state = true};
}

TEST(RecoveryTest, RecoveringNodeNeverCampaignsAndOnlyProbesTerms) {
  Raft raft(wiped(1, 3));
  for (int tick = 0; tick < 500; ++tick) {
    raft.tick();
    for (const Message& message : raft.ready().messages) {
      const auto* request = std::get_if<VoteRequest>(&message.body);
      ASSERT_NE(request, nullptr);
      EXPECT_TRUE(request->pre_vote);
    }
    ASSERT_EQ(raft.role(), Role::kFollower);
  }
  EXPECT_EQ(raft.term(), Term(0));
  EXPECT_TRUE(raft.recovering());
}

TEST(RecoveryTest, RecoveringNodeRefusesVotesAndPreVotes) {
  Raft raft(wiped(1, 3));
  raft.step({NodeId(2), NodeId(1), Term(5), VoteRequest{Index(9), Term(5), false}});
  raft.step({NodeId(3), NodeId(1), Term(6), VoteRequest{Index(9), Term(5), true}});
  const Ready ready = raft.ready();
  ASSERT_EQ(ready.messages.size(), 2U);
  for (const Message& message : ready.messages) {
    EXPECT_FALSE(std::get<VoteResponse>(message.body).granted);
  }
  EXPECT_FALSE(ready.hard_state && ready.hard_state->vote.has_value());
}

TEST(RecoveryTest, StaleLeaderAloneCannotEndRecovery) {
  Raft raft(wiped(1, 3));
  raft.step({NodeId(2), NodeId(1), Term(3), AppendRequest{Index(0), Term(0), {}, Index(0)}});
  EXPECT_TRUE(raft.recovering());
  raft.step({NodeId(3), NodeId(1), Term(5), VoteRequest{Index(0), Term(0), false}});
  const Ready ready = raft.ready();
  for (const Message& message : ready.messages) {
    if (const auto* vote = std::get_if<VoteResponse>(&message.body)) {
      EXPECT_FALSE(vote->granted);
    }
  }
}

TEST(RecoveryTest, WipedFollowerCatchesUpAndRejoins) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const std::uint32_t leader = cluster.leader_id();
  const std::uint32_t lost = leader == 1 ? 2 : 1;
  cluster.propose(leader, 30);
  cluster.tick_all();
  cluster.wipe(lost);
  EXPECT_TRUE(cluster.node(lost).recovering());
  ASSERT_TRUE(cluster.run_until([&] { return !cluster.node(lost).recovering(); }, 200));
  cluster.tick_all();
  EXPECT_EQ(cluster.leader_id(), leader);
  EXPECT_TRUE(cluster.logs_converged());
  EXPECT_EQ(cluster.applied_digest(lost), cluster.applied_digest(leader));
  EXPECT_EQ(cluster.storage(lost).state().vote, NodeId(leader));
}

// An entry committed on {1, 2, 3} of five survives node 1 losing its disk: a stale candidate
// cannot collect a vote from the recovering node to win without it.
TEST(RecoveryTest, StaleCandidateCannotWinWithAWipedVoter) {
  Cluster cluster(5, kPaperRaft);
  cluster.load(1, 2, {1, 2});
  cluster.load(2, 2, {1, 2});
  cluster.load(3, 2, {1, 2});
  cluster.load(4, 2, {1});
  cluster.load(5, 2, {1});
  cluster.wipe(1);
  cluster.isolate(2);
  cluster.isolate(3);
  for (int attempt = 0; attempt < 20; ++attempt) {
    cluster.campaign(4);
    ASSERT_NE(cluster.node(4).role(), Role::kLeader) << attempt;
    ASSERT_NE(cluster.node(5).role(), Role::kLeader) << attempt;
  }
}

TEST(RecoveryTest, HintlessRejectionLeavesProgressAlone) {
  Raft raft(
      RaftConfig{.id = NodeId(1), .members = {NodeId(1), NodeId(2), NodeId(3)}, .pre_vote = false});
  while (raft.role() != Role::kCandidate) {
    raft.tick();
  }
  raft.step({NodeId(2), NodeId(1), raft.term(), VoteResponse{true, false}});
  ASSERT_EQ(raft.role(), Role::kLeader);
  raft.ready();
  raft.step({NodeId(2), NodeId(1), raft.term(), AppendResponse{}});
  EXPECT_TRUE(raft.ready().messages.empty());
  EXPECT_EQ(raft.role(), Role::kLeader);
}

}  // namespace
}  // namespace rme::consensus
