#include "gateway/client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "testing/command_stream.hpp"
#include "tests/node/cluster.hpp"

namespace rme::gateway {
namespace {

using node::LocalCluster;

ClientOptions options(LocalCluster& cluster, std::uint64_t nonce) {
  ClientOptions out;
  for (std::uint32_t id = 1; id <= 3; ++id) {
    out.members.push_back({"127.0.0.1", cluster.client_port(id)});
  }
  out.nonce = nonce;
  return out;
}

TEST(Client, FollowsRedirectsAndSurvivesLeaderLossWithEachSeqAppliedOnce) {
  LocalCluster cluster(3);
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  const std::uint32_t first_leader = cluster.leader().value_or(1);
  ClientOptions opts = options(cluster, 11);
  opts.first = first_leader - 1;
  Client client(opts);
  client.open();
  testing::CommandStream stream(21);
  for (int i = 0; i < 100; ++i) {
    client.submit(stream.next());
  }
  cluster.stop(first_leader);
  for (int i = 0; i < 100; ++i) {
    client.submit(stream.next());
  }
  const std::uint32_t survivor = first_leader % 3 + 1;
  std::map<std::uint64_t, int> applied;
  const auto count_applied = [&] {
    applied.clear();
    for (const node::Recorded& recorded : cluster.outcomes(survivor)) {
      if (recorded.outcome.client == client.session() &&
          recorded.outcome.result == engine::SessionResult::kApplied) {
        ++applied[recorded.outcome.seq];
      }
    }
    return applied.size() == 200;
  };
  EXPECT_TRUE(LocalCluster::eventually(count_applied));
  EXPECT_GT(client.stats().reconnects, 1U);
  for (const auto& [seq, count] : applied) {
    EXPECT_EQ(count, 1) << seq;
  }
}

TEST(Client, StartsAtAFollowerAndIsRedirected) {
  LocalCluster cluster(3);
  ASSERT_TRUE(LocalCluster::eventually([&] {
    const auto leader = cluster.leader();
    return leader && cluster.node(leader.value_or(1) % 3 + 1).status().leader == leader.value_or(0);
  }));
  const std::uint32_t leader = cluster.leader().value_or(1);
  ClientOptions opts = options(cluster, 12);
  opts.first = leader % 3;
  Client client(opts);
  client.open();
  EXPECT_NE(client.session(), 0U);
  EXPECT_EQ(client.stats().redirects, 1U);
  EXPECT_EQ(client.submit(engine::Cancel{OrderId(1)}).result, engine::SessionResult::kApplied);
}

}  // namespace
}  // namespace rme::gateway
