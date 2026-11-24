#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "engine/replica.hpp"
#include "gateway/client.hpp"
#include "md/feed.hpp"

namespace rme::md {

struct SubscriberStats {
  std::uint64_t packets = 0;
  std::uint64_t bad_packets = 0;
  std::uint64_t recoveries = 0;
};

// Rebuilds the book from the feed by running its own replica over the sequenced records. A gap
// that outlasts the reorder window is closed by fetching a replica image, and the feed seq that
// follows it, from the publishing node's gateway; buffered messages past that seq then apply.
class Subscriber {
 public:
  Subscriber(std::uint16_t port, gateway::Endpoint recovery, engine::ReplicaConfig config = {});
  Subscriber(const Subscriber&) = delete;
  Subscriber& operator=(const Subscriber&) = delete;
  Subscriber(Subscriber&&) = delete;
  Subscriber& operator=(Subscriber&&) = delete;
  ~Subscriber();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  // Receives until the replica has applied through index or the timeout passes.
  bool run_until(std::uint64_t index, std::chrono::milliseconds timeout);
  void on_datagram(std::span<const std::byte> bytes);

  [[nodiscard]] std::uint64_t applied() const noexcept { return replica_.applied_index(); }
  [[nodiscard]] std::uint64_t hash() const { return replica_.hash(); }
  [[nodiscard]] const SubscriberStats& stats() const noexcept { return stats_; }

 private:
  void deliver(const Message& message);
  void drain();
  void recover();

  int fd_ = -1;
  std::uint16_t port_ = 0;
  gateway::Endpoint recovery_;
  engine::ReplicaConfig config_;
  engine::Replica replica_;
  std::uint64_t next_seq_ = 1;
  std::vector<std::byte> assembling_;
  std::map<std::uint64_t, Message> buffered_;
  std::vector<engine::Outcome> outcomes_;
  SubscriberStats stats_;
};

}  // namespace rme::md
