#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
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

Raft follower_with(std::initializer_list<std::uint64_t> terms) {
  return Raft(RaftConfig{.id = NodeId(2), .members = {NodeId(1), NodeId(2), NodeId(3)}},
              HardState{Term(5), std::nullopt}, entries(1, terms));
}

TEST(InstallSnapshotTest, LogKeepsSuffixThatAgreesWithTheSnapshot) {
  RaftLog log(entries(1, {1, 1, 2, 2}));
  log.install(Index(3), Term(2));
  EXPECT_EQ(log.snapshot_index(), Index(3));
  EXPECT_EQ(log.last_index(), Index(4));
  EXPECT_EQ(log.at(Index(4)).term, Term(2));
}

TEST(InstallSnapshotTest, LogDropsSuffixThatConflictsWithTheSnapshot) {
  RaftLog log(entries(1, {1, 1, 2, 2}));
  log.install(Index(3), Term(4));
  EXPECT_EQ(log.last_index(), Index(3));
  EXPECT_EQ(log.last_term(), Term(4));
  RaftLog shorter(entries(1, {1}));
  shorter.install(Index(9), Term(3));
  EXPECT_EQ(shorter.last_index(), Index(9));
}

TEST(InstallSnapshotTest, FollowerInstallsAndHandsTheSnapshotToTheStateMachine) {
  Raft raft = follower_with({1, 1});
  const Snapshot snapshot{Index(10), Term(4), {std::byte{7}}};
  raft.step({NodeId(1), NodeId(2), Term(5), InstallSnapshot{snapshot}});
  EXPECT_EQ(raft.commit(), Index(10));
  EXPECT_EQ(raft.applied(), Index(10));
  const Ready ready = raft.ready();
  EXPECT_EQ(ready.snapshot, std::optional(snapshot));
  EXPECT_TRUE(ready.committed.empty());
  ASSERT_EQ(ready.messages.size(), 1U);
  const auto& reply = std::get<AppendResponse>(ready.messages[0].body);
  EXPECT_TRUE(reply.success);
  EXPECT_EQ(reply.match, Index(10));

  raft.step({NodeId(1), NodeId(2), Term(5),
             AppendRequest{Index(10), Term(4), entries(11, {5}), Index(11)}});
  EXPECT_EQ(raft.log().last_index(), Index(11));
  EXPECT_EQ(raft.ready().committed.size(), 1U);
}

TEST(InstallSnapshotTest, SnapshotBehindCommitIsAcknowledgedButIgnored) {
  Raft raft = follower_with({1, 1, 1, 1});
  raft.step({NodeId(1), NodeId(2), Term(5), AppendRequest{Index(4), Term(1), {}, Index(4)}});
  raft.ready();
  raft.step({NodeId(1), NodeId(2), Term(5), InstallSnapshot{Snapshot{Index(2), Term(1), {}}}});
  const Ready ready = raft.ready();
  EXPECT_FALSE(ready.snapshot.has_value());
  EXPECT_EQ(raft.log().snapshot_index(), Index(0));
  ASSERT_EQ(ready.messages.size(), 1U);
  EXPECT_EQ(std::get<AppendResponse>(ready.messages[0].body).match, Index(4));
}

TEST(InstallSnapshotTest, FollowerBehindCompactionCatchesUp) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const std::uint32_t leader = cluster.leader_id();
  const std::uint32_t lagging = leader == 1 ? 2 : 1;
  const std::uint32_t other = 6 - leader - lagging;
  cluster.isolate(lagging);
  cluster.propose(leader, 40);
  cluster.tick_all();
  cluster.compact(leader);
  cluster.compact(other);
  cluster.propose(leader, 5);
  cluster.heal();
  ASSERT_TRUE(cluster.run_until([&] { return cluster.logs_converged(); }, 100));
  cluster.tick_all();

  const auto& snapshot = cluster.storage(lagging).snapshot();
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot.value_or(Snapshot{}).index, cluster.node(leader).log().snapshot_index());
  EXPECT_EQ(cluster.applied_digest(lagging), cluster.applied_digest(leader));
  EXPECT_EQ(cluster.applied_count(lagging), cluster.applied_count(leader));

  cluster.restart(lagging);
  cluster.propose(leader, 3);
  cluster.tick_all();
  EXPECT_EQ(cluster.applied_digest(lagging), cluster.applied_digest(leader));
}

}  // namespace
}  // namespace rme::consensus
