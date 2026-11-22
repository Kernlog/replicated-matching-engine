#include "sim/swarm.hpp"

#include <string>

#include "core/rng.hpp"

namespace rme::sim {

namespace {

std::uint32_t rate(Rng& rng, bool on, std::uint64_t max_per_mille) {
  return on ? static_cast<std::uint32_t>(1 + rng.below(max_per_mille)) : 0U;
}

}  // namespace

SimConfig swarm(std::uint64_t seed) {
  Rng rng(seed ^ 0x5A5A5A5A5A5A5A5AULL);
  SimConfig config;
  config.seed = seed;
  config.nodes = rng.chance(1, 4) ? 5 : 3;
  config.network.min_latency = Time(1);
  config.network.max_latency = Time(2 + rng.below(40));
  config.network.drop = rate(rng, rng.chance(1, 2), 200);
  config.network.delay = rate(rng, rng.chance(1, 2), 100);
  config.network.delay_spike = Time(20 + rng.below(200));
  config.network.duplicate = rate(rng, rng.chance(1, 2), 100);
  config.network.reorder = rate(rng, rng.chance(1, 2), 100);
  config.network.partitions = rng.chance(1, 2);
  config.network.min_partition_gap = Time(500 + rng.below(3000));
  config.network.max_partition_gap = config.network.min_partition_gap + Time(rng.below(8000));
  config.network.min_partition = Time(50 + rng.below(500));
  config.network.max_partition = config.network.min_partition + Time(rng.below(3000));
  config.faults.crash_one_in =
      rng.chance(1, 2) ? static_cast<std::uint32_t>(200 + rng.below(5000)) : 0;
  config.faults.sync_crash_one_in =
      rng.chance(1, 2) ? static_cast<std::uint32_t>(100 + rng.below(5000)) : 0;
  config.faults.torn_writes = rng.chance(1, 2);
  config.faults.min_down = Time(20 + rng.below(200));
  config.faults.max_down = config.faults.min_down + Time(rng.below(3000));
  config.workload.propose_per_mille = static_cast<std::uint32_t>(50 + rng.below(950));
  config.workload.max_batch = static_cast<std::uint32_t>(1 + rng.below(16));
  return config;
}

std::string describe(const SimConfig& config) {
  const auto& n = config.network;
  const auto& f = config.faults;
  return "nodes=" + std::to_string(config.nodes) +
         " latency=" + std::to_string(n.min_latency.value()) + ".." +
         std::to_string(n.max_latency.value()) + " drop=" + std::to_string(n.drop) +
         " delay=" + std::to_string(n.delay) + " dup=" + std::to_string(n.duplicate) +
         " reorder=" + std::to_string(n.reorder) + " partitions=" + (n.partitions ? "on" : "off") +
         " crash=1/" + std::to_string(f.crash_one_in) + " sync_crash=1/" +
         std::to_string(f.sync_crash_one_in) + " torn=" + (f.torn_writes ? "on" : "off") +
         " propose=" + std::to_string(config.workload.propose_per_mille) + "/1000";
}

}  // namespace rme::sim
