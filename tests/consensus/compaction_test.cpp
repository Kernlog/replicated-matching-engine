#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "consensus/raft.hpp"
#include "consensus/raft_log.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

std::vector<Entry> entries(std::uint64_t first, std::initializer_list<std::uint64_t> terms) {
  std::vector<Entry> out;
  for (const std::uint64_t term : terms) {
    out.push_back(Entry{Term(term), Index(first + out.size()), EntryType::kCommand, {}});
  }
  return out;
}

std::uint32_t elect(Cluster& cluster) {
  EXPECT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  return cluster.leader_id();
}

TEST(CompactionTest, LogDropsPrefixAndAnswersForTheSnapshotPoint) {
  RaftLog log(entries(1, {1, 1, 2, 2, 3}));
  log.compact(Index(3));
  EXPECT_EQ(log.snapshot_index(), Index(3));
  EXPECT_EQ(log.snapshot_term(), Term(2));
  EXPECT_EQ(log.last_index(), Index(5));
  EXPECT_EQ(log.term_at(Index(3)), Term(2));
  EXPECT_EQ(log.term_at(Index(2)), std::nullopt);
  EXPECT_EQ(log.at(Index(4)).term, Term(2));
  EXPECT_TRUE(log.slice(Index(3), 10).empty());
  EXPECT_EQ(log.slice(Index(4), 10).size(), 2U);
  EXPECT_EQ(log.last_index_of(Term(2)), Index(4));
}

TEST(CompactionTest, MergeSkipsEntriesAlreadyCompacted) {
  RaftLog log(entries(1, {1, 1, 1}));
  log.compact(Index(2));
  const std::vector<Entry> request = entries(2, {1, 1, 2});
  EXPECT_EQ(log.merge(Index(1), request), Index(4));
  EXPECT_EQ(log.last_index(), Index(4));
  EXPECT_EQ(log.at(Index(4)).term, Term(2));
}

TEST(CompactionTest, RestoredLogStartsAfterTheSnapshot) {
  const RaftLog log(entries(11, {4, 5}), Index(10), Term(4));
  EXPECT_EQ(log.last_index(), Index(12));
  EXPECT_EQ(log.last_term(), Term(5));
  EXPECT_TRUE(log.matches(Index(10), Term(4)));
  EXPECT_TRUE(log.unstable().empty());
}

TEST(CompactionTest, ClusterKeepsCommittingAfterEveryNodeCompacts) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  cluster.propose(leader, 30);
  cluster.tick_all();
  for (std::uint32_t id = 1; id <= 3; ++id) {
    cluster.compact(id);
    EXPECT_EQ(cluster.node(id).log().snapshot_index(), cluster.node(id).applied()) << id;
  }
  cluster.propose(leader, 20);
  cluster.tick_all();
  EXPECT_TRUE(cluster.logs_converged());
  for (std::uint32_t id = 1; id <= 3; ++id) {
    EXPECT_EQ(cluster.applied_digest(id), cluster.applied_digest(leader)) << id;
    EXPECT_EQ(cluster.applied_count(id), cluster.applied_count(leader)) << id;
  }
}

TEST(CompactionTest, RestartFromSnapshotAndSuffixRejoins) {
  Cluster cluster(3);
  const std::uint32_t leader = elect(cluster);
  const std::uint32_t follower = leader == 1 ? 2 : 1;
  cluster.propose(leader, 25);
  cluster.tick_all();
  cluster.compact(follower);
  cluster.propose(leader, 5);
  cluster.tick_all();
  ASSERT_TRUE(cluster.storage(follower).snapshot().has_value());
  cluster.restart(follower);
  EXPECT_EQ(cluster.node(follower).applied(),
            cluster.storage(follower).snapshot().value_or(Snapshot{}).index);
  cluster.propose(leader, 5);
  cluster.tick_all();
  EXPECT_EQ(cluster.applied_digest(follower), cluster.applied_digest(leader));
  EXPECT_EQ(cluster.applied_count(follower), cluster.applied_count(leader));
}

TEST(CompactionDeathTest, CompactingNothingNewAsserts) {
  Raft raft(RaftConfig{.id = NodeId(1), .members = {NodeId(1)}});
  EXPECT_DEATH(raft.compact({}), "assertion");
}

}  // namespace
}  // namespace rme::consensus
