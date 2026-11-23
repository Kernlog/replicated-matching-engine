#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "codec/batch.hpp"
#include "codec/record.hpp"
#include "consensus/raft.hpp"
#include "testing/command_stream.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

std::vector<engine::Command> commands(std::size_t count) {
  testing::CommandStream stream(5);
  std::vector<engine::Command> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(stream.next());
  }
  return out;
}

TEST(BatchingTest, PacksCommandsPerEntryUpToByteLimit) {
  Cluster cluster(3, {.max_entry_bytes = 1024});
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const std::uint32_t leader = cluster.leader_id();
  const Index before = cluster.node(leader).log().last_index();
  const auto sent = commands(100);
  for (const auto& command : sent) {
    ASSERT_TRUE(cluster.node(leader).propose(codec::encode(command)));
  }
  cluster.deliver();
  cluster.tick_all();

  const auto log = cluster.log_of(leader);
  ASSERT_EQ(cluster.node(leader).log().last_index(), before + Index(4));
  std::vector<engine::Command> received;
  for (const Entry& entry : log) {
    if (entry.type == EntryType::kCommand) {
      EXPECT_LE(entry.data.size(), 1024U);
      EXPECT_TRUE(codec::for_each_command(
          entry.data, [&](const engine::Command& command) { received.push_back(command); }));
    }
  }
  EXPECT_EQ(received, sent);
  EXPECT_TRUE(cluster.logs_converged());
  EXPECT_EQ(cluster.applied(leader).size(), log.size());
}

TEST(BatchingTest, ProposalsOnAFollowerAreRefused) {
  Cluster cluster(3, {.max_entry_bytes = 1024});
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const std::uint32_t follower = cluster.leader_id() == 1 ? 2 : 1;
  EXPECT_FALSE(cluster.node(follower).propose(codec::encode(commands(1)[0])));
  EXPECT_FALSE(cluster.node(follower).has_ready());
}

TEST(BatchingTest, LeaderRefusesProposalsPastTheUncommittedLimit) {
  Cluster cluster(3, {.max_uncommitted = 8});
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const std::uint32_t leader = cluster.leader_id();
  cluster.isolate(leader == 1 ? 2 : 1);
  cluster.isolate(leader == 3 ? 2 : 3);
  const std::array payload{std::byte{1}};
  int accepted = 0;
  for (int i = 0; i < 100; ++i) {
    accepted += cluster.node(leader).propose(payload) ? 1 : 0;
    cluster.deliver();
  }
  const Raft& raft = cluster.node(leader);
  EXPECT_LE((raft.log().last_index() - raft.commit()).value(), 8U);
  EXPECT_LT(accepted, 10);
  cluster.heal();
  ASSERT_TRUE(cluster.run_until([&] { return cluster.logs_converged(); }));
  cluster.tick_all();
  EXPECT_TRUE(cluster.node(leader).propose(payload));
}

TEST(BatchingTest, MalformedBatchVisitsNothing) {
  std::vector<std::byte> batch;
  for (const auto& command : commands(3)) {
    const auto record = codec::encode(command);
    batch.insert(batch.end(), record.begin(), record.end());
  }
  int visited = 0;
  const auto count = [&](const engine::Command& /*command*/) { ++visited; };
  EXPECT_TRUE(codec::for_each_command(batch, count));
  EXPECT_EQ(visited, 3);

  visited = 0;
  EXPECT_FALSE(codec::for_each_command(std::span(batch).first(batch.size() - 1), count));
  batch[codec::kRecordSize + 20] ^= std::byte{0x40};
  EXPECT_FALSE(codec::for_each_command(batch, count));
  EXPECT_EQ(visited, 0);
}

}  // namespace
}  // namespace rme::consensus
