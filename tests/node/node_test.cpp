#include "node/node.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
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
  consensus::RaftConfig raft;
  raft.max_entry_bytes = codec::kClientRecordSize;
  raft.max_uncommitted_entries = 2;
  // Slow disks must not trigger an election mid-test: a follower drops what it is sent.
  raft.election_ticks = 200;
  LocalCluster cluster(3, raft);
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

}  // namespace
}  // namespace rme::node
