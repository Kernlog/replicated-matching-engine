#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <variant>
#include <vector>

#include "consensus/node.hpp"
#include "consensus/raft.hpp"
#include "consensus/types.hpp"
#include "core/hash.hpp"
#include "core/rng.hpp"
#include "sim/clock.hpp"
#include "sim/disk.hpp"
#include "sim/event_queue.hpp"
#include "sim/network.hpp"

namespace rme::sim {
namespace {

using consensus::Entry;
using consensus::Message;
using consensus::NodeId;
using consensus::Raft;
using consensus::RaftConfig;
using consensus::Role;

constexpr std::uint32_t kNodes = 3;
constexpr Time kTickInterval = Time(10);
constexpr Time kEnd = Time(100000);

struct Trace {
  std::uint64_t hash = 0;
  std::uint64_t delivered = 0;
  std::uint64_t applied = 0;
  std::uint64_t crashes = 0;
  NetworkStats network;
};

struct Machine {
  Hasher hasher;
  std::uint64_t applied = 0;

  void apply(const Entry& entry) {
    ++applied;
    hasher.add(entry.index.value());
    hasher.add(entry.term.value());
    for (const std::byte byte : entry.data) {
      hasher.add(static_cast<std::uint64_t>(byte));
    }
  }
};

NetworkConfig sample_network(Rng& rng) {
  NetworkConfig config;
  config.min_latency = Time(1);
  config.max_latency = Time(5 + rng.below(26));
  config.drop = static_cast<std::uint32_t>(1 + rng.below(50));
  config.delay = static_cast<std::uint32_t>(1 + rng.below(50));
  config.duplicate = static_cast<std::uint32_t>(1 + rng.below(50));
  config.reorder = static_cast<std::uint32_t>(1 + rng.below(50));
  config.partitions = true;
  config.min_partition_gap = Time(2000);
  config.max_partition_gap = Time(8000);
  config.min_partition = Time(200);
  config.max_partition = Time(1500);
  return config;
}

std::vector<NodeId> members() {
  std::vector<NodeId> ids;
  ids.reserve(kNodes);
  for (std::uint32_t id = 1; id <= kNodes; ++id) {
    ids.emplace_back(id);
  }
  return ids;
}

// Three Raft nodes over the fake clock, network and disk; every choice comes from one seed.
class Simulation {
 public:
  explicit Simulation(std::uint64_t seed)
      : rng_(seed),
        clock_(queue_, kTickInterval),
        network_(queue_, sample_network(rng_), members(), rng_.next()),
        machines_(kNodes),
        up_(kNodes, true),
        down_until_(kNodes) {
    for (const NodeId id : members()) {
      configs_.push_back(RaftConfig{.id = id, .members = members(), .seed = rng_.next()});
      nodes_.emplace_back(configs_.back());
      disks_.emplace_back(rng_.next());
      clock_.start(id, Time(1 + rng_.below(10)));
    }
  }

  Trace run(Time end) {
    while (const auto event = queue_.pop_until(end)) {
      if (const auto* tick = std::get_if<Tick>(&*event)) {
        on_tick(*tick);
      } else {
        on_delivery(std::get<Delivery>(*event).message);
      }
    }
    trace_.network = network_.stats();
    for (const Machine& machine : machines_) {
      hasher_.add(machine.hasher.digest());
      trace_.applied += machine.applied;
    }
    trace_.hash = hasher_.digest();
    return trace_;
  }

 private:
  void on_tick(const Tick& tick) {
    clock_.ticked(tick);
    const std::size_t i = tick.node.value() - 1;
    if (!up_[i]) {
      if (queue_.now() >= down_until_[i]) {
        restart(i);
      }
      return;
    }
    if (rng_.chance(1, 3000)) {
      crash(i);
      return;
    }
    nodes_[i].tick();
    if (nodes_[i].role() == Role::kLeader && rng_.chance(3, 10)) {
      const std::array command{static_cast<std::byte>(rng_.below(256))};
      nodes_[i].propose(command);
    }
    process(i);
  }

  void on_delivery(const Message& message) {
    ++trace_.delivered;
    hasher_.add(queue_.now().value());
    hasher_.add(message.from.value());
    hasher_.add(message.to.value());
    hasher_.add(message.term.value());
    hasher_.add(message.body.index());
    const std::size_t i = message.to.value() - 1;
    if (up_[i]) {
      nodes_[i].step(message);
      process(i);
    }
  }

  void process(std::size_t i) {
    consensus::process_ready(nodes_[i], disks_[i], network_, machines_[i]);
  }

  void crash(std::size_t i) {
    ++trace_.crashes;
    up_[i] = false;
    disks_[i].crash(rng_.chance(1, 2) ? Crash::kTornTail : Crash::kClean);
    down_until_[i] = queue_.now() + Time(100 + rng_.below(1901));
    hasher_.add(i);
  }

  void restart(std::size_t i) {
    up_[i] = true;
    machines_[i] = Machine{};
    configs_[i].seed = rng_.next();
    nodes_[i] = Raft(configs_[i], disks_[i].state(), disks_[i].entries());
  }

  Rng rng_;
  EventQueue queue_;
  Clock clock_;
  Network network_;
  std::vector<RaftConfig> configs_;
  std::vector<Raft> nodes_;
  std::vector<Disk> disks_;
  std::vector<Machine> machines_;
  std::vector<bool> up_;
  std::vector<Time> down_until_;
  Hasher hasher_;
  Trace trace_;
};

Trace simulate(std::uint64_t seed) {
  return Simulation(seed).run(kEnd);
}

TEST(DeterminismTest, SameSeedYieldsSameTrace) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    const Trace first = simulate(seed);
    const Trace second = simulate(seed);
    EXPECT_EQ(first.hash, second.hash) << "seed " << seed;
    EXPECT_EQ(first.delivered, second.delivered) << "seed " << seed;
    EXPECT_EQ(first.applied, second.applied) << "seed " << seed;
    EXPECT_EQ(first.crashes, second.crashes) << "seed " << seed;
  }
}

TEST(DeterminismTest, DifferentSeedsYieldDifferentHashes) {
  constexpr std::uint64_t kSeeds = 32;
  std::set<std::uint64_t> hashes;
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    hashes.insert(simulate(seed).hash);
  }
  EXPECT_EQ(hashes.size(), kSeeds);
}

TEST(DeterminismTest, TraceCoversFaultsCrashesAndCommits) {
  const Trace trace = simulate(1);
  EXPECT_GT(trace.delivered, 10000U);
  EXPECT_GT(trace.applied, 1000U);
  EXPECT_GE(trace.crashes, 2U);
  EXPECT_GT(trace.network.dropped, 0U);
  EXPECT_GT(trace.network.delayed, 0U);
  EXPECT_GT(trace.network.duplicated, 0U);
  EXPECT_GT(trace.network.reordered, 0U);
  EXPECT_GT(trace.network.cut, 0U);
}

}  // namespace
}  // namespace rme::sim
