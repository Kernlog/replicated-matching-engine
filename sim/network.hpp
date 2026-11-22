#pragma once

#include <cstdint>
#include <map>
#include <utility>

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
};

struct NetworkStats {
  std::uint64_t sent = 0;
  std::uint64_t dropped = 0;
  std::uint64_t delayed = 0;
  std::uint64_t duplicated = 0;
  std::uint64_t reordered = 0;
};

// Links are FIFO unless a message is reordered, which lets it overtake everything in flight.
class Network {
 public:
  Network(EventQueue& queue, NetworkConfig config, std::uint64_t seed);

  void send(consensus::Message message);

  [[nodiscard]] const NetworkStats& stats() const noexcept { return stats_; }

 private:
  [[nodiscard]] Time latency();

  EventQueue* queue_;
  NetworkConfig config_;
  Rng rng_;
  std::map<std::pair<consensus::NodeId, consensus::NodeId>, Time> tail_;
  NetworkStats stats_;
};

static_assert(consensus::Transport<Network>);

}  // namespace rme::sim
