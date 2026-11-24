#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "sim/cluster.hpp"
#include "sim/swarm.hpp"

namespace rme::sim {
namespace {

// Every seed that ever found a bug (docs/bug-log.md), replayed on each change so the bug stays
// fixed even after the nightly range has moved on.
constexpr std::array<std::uint64_t, 8> kSeeds{29, 59, 161, 239, 291, 446, 463, 97276};

class RegressionSeed : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(RegressionSeed, KeepsEveryInvariant) {
  const SimStats stats = Cluster(swarm(GetParam())).run();
  std::string violations;
  for (const std::string& violation : stats.violations) {
    violations += violation + '\n';
  }
  EXPECT_TRUE(stats.violations.empty()) << violations;
}

INSTANTIATE_TEST_SUITE_P(BugLog, RegressionSeed, ::testing::ValuesIn(kSeeds));

}  // namespace
}  // namespace rme::sim
