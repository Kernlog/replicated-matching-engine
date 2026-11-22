#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "consensus/raft.hpp"
#include "consensus/types.hpp"
#include "core/hash.hpp"
#include "core/rng.hpp"
#include "sim/checker.hpp"
#include "sim/clock.hpp"
#include "sim/disk.hpp"
#include "sim/engine_machine.hpp"
#include "sim/event_queue.hpp"
#include "sim/network.hpp"
#include "testing/command_stream.hpp"

namespace rme::sim {

// Odds are "one in N" per node tick (crash) or per Ready (crash inside sync); 0 turns a fault off.
struct FaultConfig {
  std::uint32_t crash_one_in = 3000;
  std::uint32_t sync_crash_one_in = 0;
  bool torn_writes = true;
  Time min_down = Time(100);
  Time max_down = Time(2000);
};

struct WorkloadConfig {
  std::uint32_t propose_per_mille = 300;
  std::uint32_t max_batch = 4;
  Time stop_at = Time(~std::uint64_t{0});
};

struct SimConfig {
  std::uint64_t seed = 1;
  std::uint32_t nodes = 3;
  Time tick_interval = Time(10);
  Time duration = Time(100'000);
  // After duration every fault stops and down nodes restart; every node must then apply an entry
  // past anything that existed before the heal within this window.
  Time heal_window = Time(20'000);
  NetworkConfig network;
  FaultConfig faults;
  WorkloadConfig workload;
};

struct SimStats {
  std::uint64_t trace_hash = 0;
  std::uint64_t delivered = 0;
  std::uint64_t proposed = 0;
  std::uint64_t applied = 0;
  std::uint64_t crashes = 0;
  std::uint64_t sync_crashes = 0;
  Time recovered_after = Time(0);
  NetworkStats network;
  std::vector<std::string> violations;
};

// N Raft nodes running the matching engine over the fake clock, network and disks. Every choice
// (timing, faults, workload) comes from the seed, so a run replays exactly.
class Cluster {
 public:
  explicit Cluster(const SimConfig& config);

  SimStats run();

  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
  [[nodiscard]] const consensus::Raft& node(std::size_t i) const { return nodes_.at(i); }
  [[nodiscard]] const EngineMachine& machine(std::size_t i) const { return machines_.at(i); }
  [[nodiscard]] bool up(std::size_t i) const { return up_.at(i); }
  [[nodiscard]] Time now() const noexcept { return queue_.now(); }
  [[nodiscard]] const Checker& checker() const noexcept { return checker_; }

 private:
  struct Wire {
    Network* network = nullptr;
    const Disk* disk = nullptr;
    void send(consensus::Message&& message) const;
  };

  struct Applier {
    EngineMachine* machine = nullptr;
    const Disk* disk = nullptr;
    Checker* checker = nullptr;
    consensus::NodeId node;
    void apply(const consensus::Entry& entry) const;
  };

  void on_tick(const Tick& tick);
  void on_delivery(const consensus::Message& message);
  void propose(std::size_t i);
  void process(std::size_t i);
  void observe(std::size_t i);
  void check_logs();
  void heal();
  [[nodiscard]] bool recovered() const;
  bool step_until(Time end, bool stop_on_recovery);
  void crash(std::size_t i, Crash mode);
  void restart(std::size_t i);
  [[nodiscard]] bool roll(std::uint32_t one_in);

  SimConfig config_;
  Rng rng_;
  EventQueue queue_;
  Clock clock_;
  Network network_;
  testing::CommandStream workload_;
  std::vector<consensus::RaftConfig> configs_;
  std::vector<consensus::Raft> nodes_;
  std::vector<Disk> disks_;
  std::vector<EngineMachine> machines_;
  std::vector<bool> up_;
  std::vector<Time> down_until_;
  std::vector<consensus::Role> roles_;
  Checker checker_;
  std::uint64_t events_ = 0;
  bool healed_ = false;
  consensus::Index heal_mark_;
  Hasher trace_;
  SimStats stats_;
};

}  // namespace rme::sim
