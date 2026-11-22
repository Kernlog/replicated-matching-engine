#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
#include <vector>

#include "consensus/raft.hpp"
#include "consensus/raft_log.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

RaftLog log_with_terms(std::initializer_list<std::uint64_t> terms) {
  std::vector<Entry> entries;
  for (const std::uint64_t term : terms) {
    entries.push_back(Entry{Term(term), Index(entries.size() + 1), EntryType::kCommand, {}});
  }
  return RaftLog(std::move(entries));
}

AppendResponse reject_from(std::initializer_list<std::uint64_t> follower_terms,
                           std::uint64_t prev_index, std::uint64_t prev_term) {
  std::vector<Entry> entries;
  for (const std::uint64_t term : follower_terms) {
    entries.push_back(Entry{Term(term), Index(entries.size() + 1), EntryType::kCommand, {}});
  }
  Raft raft(RaftConfig{.id = NodeId(2), .members = {NodeId(1), NodeId(2), NodeId(3)}},
            HardState{Term(9), std::nullopt}, std::move(entries));
  raft.step({NodeId(1), NodeId(2), Term(9),
             AppendRequest{Index(prev_index), Term(prev_term), {}, Index(0)}});
  const Ready ready = raft.ready();
  EXPECT_EQ(ready.messages.size(), 1U);
  return std::get<AppendResponse>(ready.messages.at(0).body);
}

TEST(ConsistencyTest, LogFindsTermBoundaries) {
  const RaftLog log = log_with_terms({1, 1, 4, 4, 4, 5});
  EXPECT_EQ(log.first_index_of_term_at(Index(5)), Index(3));
  EXPECT_EQ(log.first_index_of_term_at(Index(2)), Index(1));
  EXPECT_EQ(log.last_index_of(Term(4)), Index(5));
  EXPECT_EQ(log.last_index_of(Term(1)), Index(2));
  EXPECT_EQ(log.last_index_of(Term(3)), std::nullopt);
}

TEST(ConsistencyTest, ShortFollowerReportsItsLength) {
  const AppendResponse reject = reject_from({1, 1, 2}, 7, 3);
  EXPECT_FALSE(reject.success);
  EXPECT_EQ(reject.conflict_index, Index(4));
  EXPECT_EQ(reject.conflict_term, Term(0));
}

TEST(ConsistencyTest, ConflictingFollowerReportsFirstIndexOfItsTerm) {
  const AppendResponse reject = reject_from({1, 1, 4, 4, 4, 4}, 6, 5);
  EXPECT_FALSE(reject.success);
  EXPECT_EQ(reject.conflict_term, Term(4));
  EXPECT_EQ(reject.conflict_index, Index(3));
}

TEST(ConsistencyTest, LongConflictingSuffixResolvesInOneRoundTripPerTerm) {
  Cluster cluster(3);
  cluster.load(1, 3, {1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3});
  cluster.load(2, 3, {1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2});
  cluster.load(3, 3, {1});
  cluster.campaign(1);
  ASSERT_EQ(cluster.leader_id(), 1U);
  EXPECT_TRUE(cluster.logs_converged());
  EXPECT_LE(cluster.rejections(), 2);
}

TEST(ConsistencyTest, LeaderSkipsPastEntriesOfASharedTerm) {
  Cluster cluster(3);
  cluster.load(1, 5, {1, 2, 2, 5, 5, 5});
  cluster.load(2, 5, {1, 2, 2, 2, 2, 2, 2, 2, 2});
  cluster.load(3, 5, {1, 2, 2});
  cluster.campaign(1);
  ASSERT_EQ(cluster.leader_id(), 1U);
  EXPECT_TRUE(cluster.logs_converged());
  EXPECT_LE(cluster.rejections(), 2);
}

}  // namespace
}  // namespace rme::consensus
