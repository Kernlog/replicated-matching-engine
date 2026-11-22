#include <gtest/gtest.h>

#include <cstdint>
#include <set>

#include "core/rng.hpp"
#include "sim/cluster.hpp"

namespace rme::sim {
namespace {

SimConfig faulty(std::uint64_t seed) {
  Rng rng(seed);
  SimConfig config;
  config.seed = seed;
  config.network.max_latency = Time(5 + rng.below(26));
  config.network.drop = static_cast<std::uint32_t>(1 + rng.below(50));
  config.network.delay = static_cast<std::uint32_t>(1 + rng.below(50));
  config.network.duplicate = static_cast<std::uint32_t>(1 + rng.below(50));
  config.network.reorder = static_cast<std::uint32_t>(1 + rng.below(50));
  config.network.partitions = true;
  config.network.min_partition_gap = Time(2000);
  config.network.max_partition_gap = Time(8000);
  config.network.min_partition = Time(200);
  config.network.max_partition = Time(1500);
  config.faults.sync_crash_one_in = 2000;
  return config;
}

SimStats simulate(std::uint64_t seed) {
  return Cluster(faulty(seed)).run();
}

TEST(DeterminismTest, SameSeedYieldsSameTrace) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    const SimStats first = simulate(seed);
    const SimStats second = simulate(seed);
    EXPECT_EQ(first.trace_hash, second.trace_hash) << "seed " << seed;
    EXPECT_EQ(first.delivered, second.delivered) << "seed " << seed;
    EXPECT_EQ(first.applied, second.applied) << "seed " << seed;
    EXPECT_EQ(first.crashes, second.crashes) << "seed " << seed;
  }
}

TEST(DeterminismTest, DifferentSeedsYieldDifferentHashes) {
  constexpr std::uint64_t kSeeds = 32;
  std::set<std::uint64_t> hashes;
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    hashes.insert(simulate(seed).trace_hash);
  }
  EXPECT_EQ(hashes.size(), kSeeds);
}

TEST(DeterminismTest, FaultySeedsKeepEverySafetyInvariant) {
  for (std::uint64_t seed = 1; seed <= 64; ++seed) {
    const SimStats stats = simulate(seed);
    EXPECT_TRUE(stats.violations.empty()) << "seed " << seed << ": " << stats.violations.front();
  }
}

TEST(DeterminismTest, TraceCoversFaultsCrashesAndCommits) {
  const SimStats stats = simulate(1);
  EXPECT_GT(stats.delivered, 10000U);
  EXPECT_GT(stats.applied, 1000U);
  EXPECT_GE(stats.crashes, 2U);
  EXPECT_GT(stats.network.dropped, 0U);
  EXPECT_GT(stats.network.delayed, 0U);
  EXPECT_GT(stats.network.duplicated, 0U);
  EXPECT_GT(stats.network.reordered, 0U);
  EXPECT_GT(stats.network.cut, 0U);
}

}  // namespace
}  // namespace rme::sim
