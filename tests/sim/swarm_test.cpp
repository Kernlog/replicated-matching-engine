#include "sim/swarm.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>

#include "sim/cluster.hpp"

namespace rme::sim {
namespace {

TEST(SwarmTest, EachFaultTypeIsBothOnAndOffAcrossSeeds) {
  constexpr int kSeeds = 400;
  int drop = 0;
  int partitions = 0;
  int crash = 0;
  int sync_crash = 0;
  int five_nodes = 0;
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    const SimConfig config = swarm(seed);
    drop += config.network.drop > 0 ? 1 : 0;
    partitions += config.network.partitions ? 1 : 0;
    crash += config.faults.crash_one_in > 0 ? 1 : 0;
    sync_crash += config.faults.sync_crash_one_in > 0 ? 1 : 0;
    five_nodes += config.nodes == 5 ? 1 : 0;
  }
  for (const int on : {drop, partitions, crash, sync_crash}) {
    EXPECT_GT(on, kSeeds / 2 - 50);
    EXPECT_LT(on, kSeeds / 2 + 50);
  }
  EXPECT_GT(five_nodes, kSeeds / 4 - 40);
  EXPECT_LT(five_nodes, kSeeds / 4 + 40);
}

TEST(SwarmTest, ConfigIsAPureFunctionOfTheSeed) {
  EXPECT_EQ(describe(swarm(7)), describe(swarm(7)));
  std::set<std::string> seen;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    seen.insert(describe(swarm(seed)));
  }
  EXPECT_EQ(seen.size(), 100U);
}

TEST(SwarmTest, SwarmSeedsKeepEverySafetyInvariant) {
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    SimConfig config = swarm(seed);
    config.duration = Time(50'000);
    const SimStats stats = Cluster(config).run();
    EXPECT_TRUE(stats.violations.empty())
        << "seed " << seed << " (" << describe(config) << "): " << stats.violations.front();
    EXPECT_GT(stats.recovered_after, Time(0)) << "seed " << seed;
  }
}

// Seeds that found B-001 (unbounded uncommitted log under overload): they must recover after heal.
TEST(SwarmTest, RegressionSeedsRecover) {
  for (const std::uint64_t seed : {59U, 161U, 239U, 291U, 446U, 463U}) {
    const SimStats stats = Cluster(swarm(seed)).run();
    EXPECT_TRUE(stats.violations.empty()) << "seed " << seed << ": " << stats.violations.front();
    EXPECT_GT(stats.recovered_after, Time(0)) << "seed " << seed;
  }
}

}  // namespace
}  // namespace rme::sim
