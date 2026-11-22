#include "sim/cluster.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace rme::sim {
namespace {

TEST(ClusterTest, FaultFreeReplicasConvergeOnTheSameBook) {
  SimConfig config;
  config.duration = Time(20'000);
  config.workload.stop_at = Time(18'000);
  config.faults.crash_one_in = 0;
  Cluster cluster(config);
  const SimStats stats = cluster.run();
  EXPECT_GT(stats.proposed, 500U);
  EXPECT_EQ(stats.crashes, 0U);
  for (std::size_t i = 0; i < cluster.size(); ++i) {
    EXPECT_EQ(cluster.machine(i).applied(), cluster.machine(0).applied()) << i;
    EXPECT_EQ(cluster.machine(i).book_hash(), cluster.machine(0).book_hash()) << i;
    EXPECT_EQ(cluster.machine(i).events_hash(), cluster.machine(0).events_hash()) << i;
  }
  EXPECT_EQ(cluster.machine(0).commands(), stats.proposed);
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

}  // namespace
}  // namespace rme::sim
