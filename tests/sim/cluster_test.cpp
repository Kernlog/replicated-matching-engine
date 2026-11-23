#include "sim/cluster.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace rme::sim {
namespace {

TEST(ClusterTest, FaultFreeReplicasConvergeOnTheSameBook) {
  SimConfig config;
  config.duration = Time(20'000);
  config.workload.stop_at = Time(18'000);
  config.heal_window = Time(0);
  config.faults.crash_one_in = 0;
  Cluster cluster(config);
  const SimStats stats = cluster.run();
  EXPECT_GT(stats.clients.acked, 300U);
  EXPECT_EQ(stats.crashes, 0U);
  for (std::size_t i = 0; i < cluster.size(); ++i) {
    EXPECT_EQ(cluster.machine(i).applied(), cluster.machine(0).applied()) << i;
    EXPECT_EQ(cluster.machine(i).book_hash(), cluster.machine(0).book_hash()) << i;
    EXPECT_EQ(cluster.machine(i).events_hash(), cluster.machine(0).events_hash()) << i;
  }
  EXPECT_EQ(cluster.machine(0).commands(), stats.clients.acked);
  EXPECT_EQ(stats.clients.retries, 0U);
}

TEST(ClusterTest, CrashesInsideSyncRecover) {
  SimConfig config;
  config.duration = Time(50'000);
  config.workload.stop_at = Time(45'000);
  config.faults.crash_one_in = 0;
  config.faults.sync_crash_one_in = 200;
  Cluster cluster(config);
  const SimStats stats = cluster.run();
  EXPECT_GT(stats.sync_crashes, 10U);
  EXPECT_GT(stats.applied, 0U);
  std::size_t up = 0;
  for (std::size_t i = 0; i < cluster.size(); ++i) {
    up += cluster.up(i) ? 1U : 0U;
  }
  EXPECT_GE(up, 2U);
}

TEST(ClusterTest, FiveNodesRun) {
  SimConfig config;
  config.nodes = 5;
  config.duration = Time(20'000);
  const SimStats stats = Cluster(config).run();
  EXPECT_GT(stats.applied, 1000U);
}

TEST(ClusterTest, RecoversAfterHealFromHeavyFaults) {
  SimConfig config;
  config.seed = 11;
  config.duration = Time(30'000);
  config.network.drop = 300;
  config.network.partitions = true;
  config.network.min_partition_gap = Time(500);
  config.network.max_partition_gap = Time(1000);
  config.network.min_partition = Time(2000);
  config.network.max_partition = Time(4000);
  config.faults.crash_one_in = 300;
  config.faults.sync_crash_one_in = 300;
  const SimStats stats = Cluster(config).run();
  EXPECT_TRUE(stats.violations.empty()) << stats.violations.front();
  EXPECT_GT(stats.recovered_after, Time(0));
  EXPECT_LT(stats.recovered_after, Time(5'000));
}

TEST(ClusterTest, ReportsLivenessFailureWhenTheWindowIsTooShort) {
  SimConfig config;
  config.duration = Time(5'000);
  config.heal_window = Time(1);
  config.workload.stop_at = Time(0);
  const SimStats stats = Cluster(config).run();
  ASSERT_FALSE(stats.violations.empty());
  EXPECT_NE(stats.violations.front().find("liveness"), std::string::npos);
}

TEST(ClusterTest, WipedNodesRejoinThroughRecovery) {
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    SimConfig config;
    config.seed = seed;
    config.duration = Time(60'000);
    config.faults.crash_one_in = 0;
    config.faults.wipe_one_in = 400;
    config.workload.snapshot_every = 64;
    const SimStats stats = Cluster(config).run();
    EXPECT_TRUE(stats.violations.empty()) << "seed " << seed << ": " << stats.violations.front();
    EXPECT_GT(stats.wipes, 3U) << seed;
    EXPECT_GT(stats.snapshots, 3U) << seed;
    EXPECT_GT(stats.recovered_after, Time(0)) << seed;
  }
}

TEST(ClusterTest, ClientsRetryThroughLeaderChanges) {
  SimConfig config;
  config.duration = Time(60'000);
  config.network.partitions = true;
  config.network.drop = 50;
  const SimStats stats = Cluster(config).run();
  EXPECT_TRUE(stats.violations.empty()) << stats.violations.front();
  EXPECT_GT(stats.clients.retries, 0U);
  EXPECT_GT(stats.clients.acked, 100U);
}

}  // namespace
}  // namespace rme::sim
