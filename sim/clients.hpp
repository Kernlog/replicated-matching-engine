#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "codec/client_record.hpp"
#include "core/rng.hpp"
#include "engine/replica.hpp"
#include "sim/event_queue.hpp"
#include "testing/command_stream.hpp"

namespace rme::sim {

struct ClientConfig {
  std::uint32_t count = 8;
  Time retry_after = Time(300);
};

struct ClientStats {
  std::uint64_t sent = 0;
  std::uint64_t retries = 0;
  std::uint64_t registered = 0;
  std::uint64_t acked = 0;
  std::uint64_t expired = 0;
};

// Clients with Raft-thesis sessions: each registers with a nonce, then keeps one command in flight
// and re-sends the same seq if no outcome arrives in time. An outcome counts only when the leader
// the client talks to applies it, which is when a real gateway would ack.
class Clients {
 public:
  Clients(ClientConfig config, std::uint64_t seed);

  // Records to propose now: new commands from idle clients (each with the given odds) and retries.
  void collect(Time now, std::uint32_t per_mille, std::vector<codec::ClientRecordBytes>& out);
  void on_outcome(const engine::Outcome& outcome);

  [[nodiscard]] const ClientStats& stats() const noexcept { return stats_; }
  // Every (session, seq) a client has seen acknowledged, in ack order.
  [[nodiscard]] const std::vector<std::pair<std::uint64_t, std::uint64_t>>& acked() const noexcept {
    return acked_;
  }

 private:
  struct Pending {
    std::uint64_t seq = 0;
    codec::ClientRecordBytes record{};
    Time sent;
  };

  struct Client {
    std::uint64_t nonce = 0;
    std::optional<std::uint64_t> session;
    std::uint64_t acked_seq = 0;
    std::optional<Pending> pending;
  };

  ClientConfig config_;
  Rng rng_;
  testing::CommandStream stream_;
  std::vector<Client> clients_;
  std::uint64_t next_nonce_ = 1;
  ClientStats stats_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> acked_;
};

}  // namespace rme::sim
