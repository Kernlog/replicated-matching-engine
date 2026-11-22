#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
#include <vector>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

std::vector<Entry> entries_with_terms(std::initializer_list<std::uint64_t> terms) {
  std::vector<Entry> out;
  for (const std::uint64_t term : terms) {
    out.push_back(Entry{Term(term), Index(out.size() + 1), EntryType::kCommand, {}});
  }
  return out;
}

Raft follower(HardState state = {}, std::vector<Entry> log = {}) {
  return Raft(RaftConfig{.id = NodeId(1), .members = {NodeId(1), NodeId(2), NodeId(3)}}, state,
              std::move(log));
}

template <typename Body>
Body only_reply(Raft& raft) {
  const Ready ready = raft.ready();
  EXPECT_EQ(ready.messages.size(), 1U);
  return std::get<Body>(ready.messages.at(0).body);
}

Message vote_request(std::uint32_t from, std::uint64_t term, std::uint64_t last_index,
                     std::uint64_t last_term) {
  return {NodeId(from), NodeId(1), Term(term), VoteRequest{Index(last_index), Term(last_term)}};
}

TEST(ElectionTest, SingleNodeElectsItself) {
  Cluster cluster(1);
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader().has_value(); }));
  EXPECT_EQ(cluster.node(1).term(), Term(1));
}

TEST(ElectionTest, ThreeNodesAgreeOnOneLeader) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    Cluster cluster(3, seed);
    ASSERT_TRUE(cluster.run_until([&] { return cluster.leader().has_value(); })) << seed;
    const std::uint32_t leader = cluster.leader_id();
    for (std::uint32_t id = 1; id <= 3; ++id) {
      EXPECT_EQ(cluster.node(id).leader(), NodeId(leader)) << seed;
      EXPECT_EQ(cluster.node(id).term(), cluster.node(leader).term()) << seed;
    }
  }
}

TEST(ElectionTest, HeartbeatsKeepLeaderInPlace) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader().has_value(); }));
  const std::uint32_t leader = cluster.leader_id();
  const Term term = cluster.node(leader).term();
  for (int i = 0; i < 200; ++i) {
    cluster.tick_all();
  }
  EXPECT_EQ(cluster.leader(), leader);
  EXPECT_EQ(cluster.node(leader).term(), term);
}

TEST(ElectionTest, SplitVoteIsResolvedInALaterTerm) {
  Cluster cluster(4);
  cluster.partition({{1, 3}, {2, 4}});
  cluster.campaign(1);
  cluster.campaign(2);
  EXPECT_EQ(cluster.node(1).role(), Role::kCandidate);
  EXPECT_EQ(cluster.node(2).role(), Role::kCandidate);
  EXPECT_FALSE(cluster.leader().has_value());
  cluster.heal();
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader().has_value(); }));
  EXPECT_GT(cluster.node(cluster.leader_id()).term(), Term(1));
}

TEST(ElectionTest, IsolatedLeaderIsReplacedAndStepsDownOnReturn) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader().has_value(); }));
  const std::uint32_t old_leader = cluster.leader_id();
  cluster.isolate(old_leader);
  ASSERT_TRUE(cluster.run_until([&] {
    const auto leader = cluster.leader();
    return leader && *leader != old_leader;
  }));
  cluster.heal();
  cluster.tick_all();
  EXPECT_EQ(cluster.node(old_leader).role(), Role::kFollower);
  EXPECT_EQ(cluster.node(old_leader).term(), cluster.node(cluster.leader_id()).term());
}

TEST(ElectionTest, RejectsCandidateWithStaleLog) {
  Raft raft = follower({Term(2), std::nullopt}, entries_with_terms({1, 2}));
  raft.step(vote_request(2, 3, 5, 1));
  EXPECT_FALSE(only_reply<VoteResponse>(raft).granted);
  raft.step(vote_request(3, 3, 1, 2));
  EXPECT_FALSE(only_reply<VoteResponse>(raft).granted);
  raft.step(vote_request(3, 3, 2, 2));
  EXPECT_TRUE(only_reply<VoteResponse>(raft).granted);
}

TEST(ElectionTest, HigherLastTermWinsOverLongerLog) {
  Raft raft = follower({Term(2), std::nullopt}, entries_with_terms({1, 1, 1}));
  raft.step(vote_request(2, 3, 1, 2));
  EXPECT_TRUE(only_reply<VoteResponse>(raft).granted);
}

TEST(ElectionTest, VotesAtMostOncePerTerm) {
  Raft raft = follower();
  raft.step(vote_request(2, 1, 0, 0));
  EXPECT_TRUE(only_reply<VoteResponse>(raft).granted);
  raft.step(vote_request(3, 1, 0, 0));
  EXPECT_FALSE(only_reply<VoteResponse>(raft).granted);
  raft.step(vote_request(2, 1, 0, 0));
  EXPECT_TRUE(only_reply<VoteResponse>(raft).granted);
  raft.step(vote_request(3, 2, 0, 0));
  EXPECT_TRUE(only_reply<VoteResponse>(raft).granted);
}

TEST(ElectionTest, StaleAppendIsAnsweredWithCurrentTerm) {
  Raft raft = follower({Term(3), std::nullopt});
  raft.step({NodeId(2), NodeId(1), Term(2), AppendRequest{}});
  const Ready ready = raft.ready();
  ASSERT_EQ(ready.messages.size(), 1U);
  EXPECT_EQ(ready.messages[0].term, Term(3));
  EXPECT_FALSE(std::get<AppendResponse>(ready.messages[0].body).success);
}

TEST(ElectionTest, HardStateIsEmittedOncePerChange) {
  Raft raft = follower();
  raft.step(vote_request(2, 1, 0, 0));
  EXPECT_EQ(raft.ready().hard_state, std::optional(HardState{Term(1), NodeId(2)}));
  raft.step(vote_request(2, 1, 0, 0));
  EXPECT_FALSE(raft.ready().hard_state.has_value());
}

TEST(ElectionTest, DropsMessagesFromNonMembersOrForOtherNodes) {
  Raft raft = follower();
  raft.step({NodeId(9), NodeId(1), Term(5), VoteRequest{}});
  raft.step({NodeId(2), NodeId(3), Term(5), VoteRequest{}});
  raft.step({NodeId(1), NodeId(1), Term(5), VoteRequest{}});
  EXPECT_EQ(raft.dropped(), 3U);
  EXPECT_EQ(raft.term(), Term(0));
  EXPECT_FALSE(raft.has_ready());
}

}  // namespace
}  // namespace rme::consensus
