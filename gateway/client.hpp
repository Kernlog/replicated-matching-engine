#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codec/client_record.hpp"
#include "engine/command.hpp"
#include "gateway/protocol.hpp"
#include "io/frame.hpp"

namespace rme::gateway {

struct Endpoint {
  std::string host;
  std::uint16_t port = 0;
};

struct ClientOptions {
  // Client ports of every member in id order, so a redirect's leader id picks one.
  std::vector<Endpoint> members;
  std::size_t first = 0;
  std::uint64_t nonce = 1;
  std::chrono::milliseconds attempt_timeout{300};
  std::chrono::milliseconds give_up_after{std::chrono::seconds(30)};
};

struct ClientStats {
  std::uint64_t attempts = 0;
  std::uint64_t redirects = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t reconnects = 0;
};

// Blocking client with one command in flight. A command is resent with the same seq until some
// leader acks it, following redirects and moving on from nodes that time out; the session dedup
// on the replicas makes the resends safe, and a duplicate's ack carries the original events.
class Client {
 public:
  explicit Client(ClientOptions options);

  // Opens a session; throws if no leader acks within give_up_after.
  void open();
  Ack submit(const engine::Command& command);

  [[nodiscard]] std::uint64_t session() const noexcept { return session_; }
  // The member the last ack came from.
  [[nodiscard]] std::size_t target() const noexcept { return target_; }
  [[nodiscard]] const ClientStats& stats() const noexcept { return stats_; }

 private:
  enum class Reply : std::uint8_t { kAck, kRedirect, kTimeout, kBroken };

  Ack until_acked(const codec::ClientRecordBytes& record, std::uint64_t client, std::uint64_t seq);
  bool connect();
  Reply await_reply(std::uint64_t client, std::uint64_t seq, Ack& ack, std::uint32_t& leader);
  static std::optional<Reply> classify(const io::Frame& frame, std::uint64_t client,
                                       std::uint64_t seq, Ack& ack, std::uint32_t& leader);
  void move_on(std::optional<std::uint32_t> leader);

  ClientOptions options_;
  asio::io_context io_;
  asio::ip::tcp::socket socket_;
  io::FrameDecoder decoder_{std::size_t{64} << 10U};
  std::size_t target_ = 0;
  bool connected_ = false;
  std::uint64_t session_ = 0;
  std::uint64_t next_seq_ = 1;
  ClientStats stats_;
};

}  // namespace rme::gateway
