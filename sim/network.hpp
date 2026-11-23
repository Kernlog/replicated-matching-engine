#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "consensus/concepts.hpp"
#include "consensus/types.hpp"
#include "core/rng.hpp"
#include "sim/event_queue.hpp"

namespace rme::sim {

// Fault rates are per mille of sent messages.
struct NetworkConfig {
  Time min_latency = Time(1);
  Time max_latency = Time(10);
  std::uint32_t drop = 0;
  std::uint32_t delay = 0;
  Time delay_spike = Time(50);
  std::uint32_t duplicate = 0;
  std::uint32_t reorder = 0;
  bool partitions = false;
  Time min_partition_gap = Time(200);
  Time max_partition_gap = Time(1000);
  Time min_partition = Time(50);
  Time max_partition = Time(300);
};

struct NetworkStats {
  std::uint64_t sent = 0;
  std::uint64_t dropped = 0;
  std::uint64_t delayed = 0;
  std::uint64_t duplicated = 0;
  std::uint64_t reordered = 0;
  std::uint64_t cut = 0;
};

// Links are FIFO unless a message is reordered, which lets it overtake everything in flight. A
// partition cuts a random minority off from the rest for a sampled duration, then heals for a
// sampled gap.
class Network {
 public:
  Network(EventQueue& queue, NetworkConfig config, std::vector<consensus::NodeId> nodes,
          std::uint64_t seed);

  void send(consensus::Message message);
  // Ends every fault: no loss, delay spikes, duplication, reordering or partitions from now on.
  void heal();

  [[nodiscard]] bool connected(consensus::NodeId a, consensus::NodeId b) const;
  [[nodiscard]] const NetworkStats& stats() const noexcept { return stats_; }

 private:
  void update_partition();
  void split();
  [[nodiscard]] Time sample(Time low, Time high);

  EventQueue* queue_;
  NetworkConfig config_;
  std::vector<consensus::NodeId> nodes_;
  Rng rng_;
  std::vector<consensus::NodeId> isolated_;
  Time next_change_;
  std::map<std::pair<consensus::NodeId, consensus::NodeId>, Time> tail_;
  NetworkStats stats_;
};

static_assert(consensus::Transport<Network>);

}  // namespace rme::sim
