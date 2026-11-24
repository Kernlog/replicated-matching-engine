#pragma once

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "core/spsc.hpp"
#include "gateway/protocol.hpp"
#include "node/node.hpp"

namespace rme::gateway {

// Client-facing TCP endpoint of a node, on its own Asio thread. Submissions go to the node's core;
// an ack goes back only after the command's entry is committed and applied here, so an acked
// command survives any minority of failures.
class Gateway {
 public:
  Gateway(const std::string& host, std::uint16_t port);
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;
  Gateway(Gateway&&) = delete;
  Gateway& operator=(Gateway&&) = delete;
  ~Gateway();

  // Runs on the node's core thread; pass it to the Node before start.
  [[nodiscard]] node::Node::OutcomeHandler outcome_handler();

  void start(node::Node& node);
  void stop();

  [[nodiscard]] std::uint16_t port() const;

 private:
  struct Connection;

  asio::awaitable<void> accept();
  asio::awaitable<void> serve(std::shared_ptr<Connection> connection);
  asio::awaitable<void> write_loop(std::shared_ptr<Connection> connection);
  void on_submit(const std::shared_ptr<Connection>& connection, std::span<const std::byte> body);
  void drain();

  asio::io_context io_{1};
  asio::ip::tcp::acceptor acceptor_;
  node::Node* node_ = nullptr;
  SpscQueue<Ack> acks_{8192};
  alignas(kFalseSharingRange) std::atomic<bool> drain_posted_{false};
  std::map<std::uint64_t, std::weak_ptr<Connection>> registering_;
  std::map<std::uint64_t, std::weak_ptr<Connection>> sessions_;
  std::jthread thread_;
};

}  // namespace rme::gateway
