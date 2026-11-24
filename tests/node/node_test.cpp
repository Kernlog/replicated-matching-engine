#include "node/node.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include "codec/client_record.hpp"
#include "testing/command_stream.hpp"
#include "tests/node/cluster.hpp"

namespace rme::node {
namespace {

std::vector<std::byte> bytes(const codec::ClientRecordBytes& record) {
  return {record.begin(), record.end()};
}

// Registers a session through the leader and returns its id.
std::uint64_t register_client(LocalCluster& cluster, std::uint32_t leader, std::uint64_t nonce) {
  cluster.node(leader).submit(bytes(codec::encode_register(nonce)));
  std::uint64_t session = 0;
  EXPECT_TRUE(LocalCluster::eventually([&] {
    for (const Recorded& recorded : cluster.outcomes(leader)) {
      if (recorded.outcome.result == engine::SessionResult::kRegistered &&
          recorded.outcome.seq == nonce) {
        session = recorded.outcome.client;
        return true;
      }
    }
    return false;
  }));
  return session;
}

std::size_t applied_commands(LocalCluster& cluster, std::uint32_t id, std::uint64_t session) {
  const std::vector<Recorded> outcomes = cluster.outcomes(id);
  return static_cast<std::size_t>(std::ranges::count_if(outcomes, [&](const Recorded& recorded) {
    return recorded.outcome.client == session &&
           recorded.outcome.result == engine::SessionResult::kApplied;
  }));
}

void submit_commands(LocalCluster& cluster, std::uint32_t leader, std::uint64_t session,
                     std::uint64_t from, std::uint64_t count, testing::CommandStream& stream) {
  for (std::uint64_t seq = from; seq < from + count; ++seq) {
    cluster.node(leader).submit(bytes(codec::encode(
        engine::ClientCommand{.client = session, .seq = seq, .command = stream.next()})));
  }
}

bool converged(LocalCluster& cluster, std::uint32_t size) {
  const NodeStatus first = cluster.node(1).status();
  for (std::uint32_t id = 2; id <= size; ++id) {
    const NodeStatus status = cluster.node(id).status();
    if (status.applied != first.applied || status.replica_hash != first.replica_hash) {
      return false;
    }
  }
  return first.applied != 0;
}

TEST(Node, ThreeNodesElectReplicateAndAgreeOnTheBook) {
  LocalCluster cluster(3);
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  const std::uint32_t leader = cluster.leader().value_or(1);
  const std::uint64_t session = register_client(cluster, leader, 77);
  testing::CommandStream stream(5);
  submit_commands(cluster, leader, session, 1, 200, stream);
  ASSERT_TRUE(LocalCluster::eventually(
      [&] { return applied_commands(cluster, leader, session) == 200 && converged(cluster, 3); }));
  for (std::uint32_t id = 1; id <= 3; ++id) {
    EXPECT_EQ(applied_commands(cluster, id, session), 200U) << id;
  }
}

TEST(Node, RestartedFollowerRecoversFromDiskAndCatchesUp) {
  LocalCluster cluster(3);
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  const std::uint32_t leader = cluster.leader().value_or(1);
  const std::uint32_t follower = leader % 3 + 1;
  const std::uint64_t session = register_client(cluster, leader, 5);
  testing::CommandStream stream(9);
  submit_commands(cluster, leader, session, 1, 100, stream);
  ASSERT_TRUE(LocalCluster::eventually([&] { return converged(cluster, 3); }));
  cluster.stop(follower);
  submit_commands(cluster, leader, session, 101, 100, stream);
  ASSERT_TRUE(
      LocalCluster::eventually([&] { return applied_commands(cluster, leader, session) == 200; }));
  cluster.start(follower);
  EXPECT_TRUE(LocalCluster::eventually([&] { return converged(cluster, 3); }));
}

// A leader at its uncommitted limit must hold commands back, not drop them: a dropped seq would
// make every later command of the session out of order.
TEST(Node, LeaderAtItsUncommittedLimitHoldsCommandsInsteadOfDroppingThem) {
  NodeConfig base;
  base.raft.max_entry_bytes = codec::kClientRecordSize;
  base.raft.max_uncommitted_entries = 2;
  // Slow disks must not trigger an election mid-test: a follower drops what it is sent.
  base.raft.election_ticks = 200;
  LocalCluster cluster(3, {}, base);
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  const std::uint32_t leader = cluster.leader().value_or(1);
  const std::uint64_t session = register_client(cluster, leader, 3);
  testing::CommandStream stream(2);
  submit_commands(cluster, leader, session, 1, 500, stream);
  ASSERT_TRUE(
      LocalCluster::eventually([&] { return applied_commands(cluster, leader, session) == 500; }));
  const std::vector<Recorded> outcomes = cluster.outcomes(leader);
  EXPECT_EQ(std::ranges::count_if(outcomes,
                                  [](const Recorded& recorded) {
                                    return recorded.outcome.result ==
                                           engine::SessionResult::kOutOfOrder;
                                  }),
            0);
}

NodeConfig compacting() {
  NodeConfig base;
  base.snapshot_every = 64;
  base.raft.max_entry_bytes = 4 * codec::kClientRecordSize;
  return base;
}

TEST(Node, CompactsTheLogAndKeepsAgreeing) {
  LocalCluster cluster(3, {}, compacting());
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  const std::uint32_t leader = cluster.leader().value_or(1);
  const std::uint64_t session = register_client(cluster, leader, 21);
  testing::CommandStream stream(4);
  submit_commands(cluster, leader, session, 1, 600, stream);
  ASSERT_TRUE(LocalCluster::eventually(
      [&] { return applied_commands(cluster, leader, session) == 600 && converged(cluster, 3); }));
  for (std::uint32_t id = 1; id <= 3; ++id) {
    EXPECT_GE(cluster.node(id).status().snapshot_index, 64U) << id;
  }
}

// A follower that misses more than a snapshot interval restarts from its own snapshot and WAL,
// then takes the leader's snapshot for the entries the leader no longer holds.
TEST(Node, FollowerBehindTheLeadersSnapshotCatchesUp) {
  LocalCluster cluster(3, {}, compacting());
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  const std::uint32_t leader = cluster.leader().value_or(1);
  const std::uint32_t follower = leader % 3 + 1;
  const std::uint64_t session = register_client(cluster, leader, 22);
  testing::CommandStream stream(6);
  submit_commands(cluster, leader, session, 1, 300, stream);
  ASSERT_TRUE(LocalCluster::eventually([&] { return converged(cluster, 3); }));
  const std::uint64_t stopped_at = cluster.node(follower).status().applied;
  cluster.stop(follower);
  submit_commands(cluster, leader, session, 301, 600, stream);
  ASSERT_TRUE(LocalCluster::eventually(
      [&] { return cluster.node(leader).status().snapshot_index > stopped_at + 64; }));
  cluster.start(follower);
  EXPECT_TRUE(LocalCluster::eventually([&] { return converged(cluster, 3); }));
}

// A follower that lost its disk comes back with only the operator's marker: it rejoins without
// voting, catches up, and drops the marker once a leader of the current term confirms its log.
TEST(Node, WipedFollowerRecoversAndClearsItsMarker) {
  LocalCluster cluster(3, {}, compacting());
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  const std::uint32_t leader = cluster.leader().value_or(1);
  const std::uint32_t follower = leader % 3 + 1;
  const std::uint64_t session = register_client(cluster, leader, 24);
  testing::CommandStream stream(10);
  submit_commands(cluster, leader, session, 1, 300, stream);
  ASSERT_TRUE(LocalCluster::eventually([&] { return converged(cluster, 3); }));
  cluster.wipe(follower);
  submit_commands(cluster, leader, session, 301, 100, stream);
  ASSERT_TRUE(LocalCluster::eventually([&] { return converged(cluster, 3); }));
  EXPECT_TRUE(LocalCluster::eventually([&] {
    return !cluster.node(follower).status().recovering &&
           !std::filesystem::exists(cluster.data_dir(follower) / "LOST_STATE");
  }));
}

// Feed seqs come from the replica, which snapshots carry, so a follower that restarts from its own
// snapshot and then installs the leader's numbers every entry exactly as the leader did.
TEST(Node, FeedSeqsSurviveSnapshotsAndRestarts) {
  std::mutex mutex;
  std::array<std::map<std::uint64_t, std::uint64_t>, 3> seqs;
  LocalCluster cluster(
      3,
      [&](std::uint32_t id, Node& node, gateway::Gateway&) {
        node.on_entry([&, id](const consensus::Entry& entry, std::uint64_t first_seq) {
          const std::scoped_lock lock(mutex);
          seqs.at(id - 1)[entry.index.value()] = first_seq;
        });
      },
      compacting());
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  const std::uint32_t leader = cluster.leader().value_or(1);
  const std::uint32_t follower = leader % 3 + 1;
  const std::uint64_t session = register_client(cluster, leader, 23);
  testing::CommandStream stream(8);
  submit_commands(cluster, leader, session, 1, 200, stream);
  ASSERT_TRUE(LocalCluster::eventually([&] { return converged(cluster, 3); }));
  cluster.stop(follower);
  submit_commands(cluster, leader, session, 201, 400, stream);
  ASSERT_TRUE(
      LocalCluster::eventually([&] { return applied_commands(cluster, leader, session) == 600; }));
  cluster.start(follower);
  ASSERT_TRUE(LocalCluster::eventually([&] { return converged(cluster, 3); }));
  // The follower is now past the leader's snapshot, so what it applies next is numbered from the
  // replica state that snapshot carried.
  const std::uint64_t rejoined_at = cluster.node(follower).status().applied;
  submit_commands(cluster, leader, session, 601, 100, stream);
  ASSERT_TRUE(LocalCluster::eventually(
      [&] { return applied_commands(cluster, leader, session) == 700 && converged(cluster, 3); }));
  const std::scoped_lock lock(mutex);
  std::size_t after_rejoin = 0;
  for (const auto& [index, first_seq] : seqs.at(follower - 1)) {
    const auto found = seqs.at(leader - 1).find(index);
    ASSERT_NE(found, seqs.at(leader - 1).end()) << "index " << index;
    EXPECT_EQ(first_seq, found->second) << "index " << index;
    after_rejoin += index > rejoined_at ? 1U : 0U;
  }
  EXPECT_GE(after_rejoin, 20U);
}

}  // namespace
}  // namespace rme::node
