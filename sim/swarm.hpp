#pragma once

#include <cstdint>
#include <string>

#include "sim/cluster.hpp"

namespace rme::sim {

// Swarm testing: each seed switches every fault type on or off independently and samples its
// rate, so runs cover sparse combinations (only partitions, only sync crashes) that a uniform mix
// of every fault would rarely isolate.
[[nodiscard]] SimConfig swarm(std::uint64_t seed);

[[nodiscard]] std::string describe(const SimConfig& config);

}  // namespace rme::sim
