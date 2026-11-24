#pragma once

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "consensus/types.hpp"
#include "core/spsc.hpp"

namespace rme::io {

struct PeerAddress {
  consensus::NodeId id;
  std::string host;
  std::uint16_t port = 0;
};

struct TransportStats {
  std::uint64_t frames_sent = 0;
  std::uint64_t writes = 0;
  std::uint64_t dropped = 0;
  std::uint64_t connects = 0;
};

// Raft messages over TCP, driven by one Asio thread. Each node dials one outbound stream per peer
// and reads whatever peers dial in. Messages to a peer that is down are dropped; Raft resends.
class Transport {
 public:
  using Handler = std::function<void(consensus::Message&&)>;

  // Binds the listener now (port 0 picks one), so peers can learn the port before start.
  Transport(consensus::NodeId self, const std::string& host, std::uint16_t port,
            Handler on_message);
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;
  Transport(Transport&&) = delete;
  Transport& operator=(Transport&&) = delete;
  ~Transport();

  [[nodiscard]] std::uint16_t port() const;

  void add_peer(PeerAddress address);
  void start();
  // Stops the network thread; nothing is delivered or sent after it returns.
  void stop();

  // Core thread: queue a message, then flush once per batch to hand the batch to the network.
  void send(consensus::Message&& message) noexcept;
  void flush();

  // Network thread only, or after the transport stopped.
  [[nodiscard]] const TransportStats& stats() const noexcept { return stats_; }

 private:
  struct Peer;

  asio::awaitable<void> accept();
  asio::awaitable<void> serve(asio::ip::tcp::socket socket);
  asio::awaitable<void> dial(Peer* peer);
  asio::awaitable<void> write_loop(Peer* peer);
  void drain();

  consensus::NodeId self_;
  Handler on_message_;
  asio::io_context io_{1};
  asio::ip::tcp::acceptor acceptor_;
  std::map<consensus::NodeId, std::unique_ptr<Peer>> peers_;
  SpscQueue<consensus::Message> outbox_{4096};
  alignas(kFalseSharingRange) std::atomic<bool> drain_posted_{false};
  std::vector<std::byte> scratch_;
  TransportStats stats_;
  std::jthread thread_;
};

}  // namespace rme::io
