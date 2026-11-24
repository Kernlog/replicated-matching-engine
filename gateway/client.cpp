#include "gateway/client.hpp"

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/write.hpp>
#include <stdexcept>

namespace rme::gateway {

Client::Client(ClientOptions options) : options_(std::move(options)), socket_(io_) {
  if (options_.members.empty()) {
    throw std::invalid_argument("client needs at least one member");
  }
  target_ = options_.first % options_.members.size();
}

void Client::open() {
  const Ack ack =
      until_acked(codec::encode_register(options_.nonce), engine::kRegisterClient, options_.nonce);
  session_ = ack.client;
}

Ack Client::submit(const engine::Command& command) {
  const std::uint64_t seq = next_seq_++;
  const Ack ack = until_acked(
      codec::encode(engine::ClientCommand{.client = session_, .seq = seq, .command = command}),
      session_, seq);
  if (ack.result != engine::SessionResult::kApplied &&
      ack.result != engine::SessionResult::kDuplicate) {
    throw std::runtime_error("command rejected by session check");
  }
  return ack;
}

Ack Client::until_acked(const codec::ClientRecordBytes& record, std::uint64_t client,
                        std::uint64_t seq) {
  const auto deadline = std::chrono::steady_clock::now() + options_.give_up_after;
  std::vector<std::byte> frame;
  encode_submit(record, frame);
  while (std::chrono::steady_clock::now() < deadline) {
    ++stats_.attempts;
    if (!connect()) {
      move_on(std::nullopt);
      continue;
    }
    asio::error_code error;
    asio::write(socket_, asio::buffer(frame), error);
    if (error) {
      move_on(std::nullopt);
      continue;
    }
    Ack ack;
    std::uint32_t leader = 0;
    switch (await_reply(client, seq, ack, leader)) {
      case Reply::kAck:
        return ack;
      case Reply::kRedirect:
        ++stats_.redirects;
        move_on(leader == 0 ? std::nullopt : std::optional(leader));
        break;
      case Reply::kTimeout:
        ++stats_.timeouts;
        move_on(std::nullopt);
        break;
      case Reply::kBroken:
        move_on(std::nullopt);
        break;
    }
  }
  throw std::runtime_error("no leader acked within the deadline");
}

bool Client::connect() {
  if (connected_) {
    return true;
  }
  ++stats_.reconnects;
  const Endpoint& endpoint = options_.members[target_];
  asio::ip::tcp::resolver resolver(io_);
  asio::error_code error;
  const auto endpoints = resolver.resolve(endpoint.host, std::to_string(endpoint.port), error);
  if (error) {
    return false;
  }
  socket_ = asio::ip::tcp::socket(io_);
  asio::connect(socket_, endpoints, error);
  if (error) {
    return false;
  }
  socket_.set_option(asio::ip::tcp::no_delay(true), error);
  decoder_ = io::FrameDecoder(std::size_t{64} << 10U);
  connected_ = true;
  return true;
}

Client::Reply Client::await_reply(std::uint64_t client, std::uint64_t seq, Ack& ack,
                                  std::uint32_t& leader) {
  const auto deadline = std::chrono::steady_clock::now() + options_.attempt_timeout;
  for (;;) {
    while (const std::optional<io::Frame> frame = decoder_.next()) {
      if (const std::optional<Reply> reply = classify(*frame, client, seq, ack, leader)) {
        return *reply;
      }
    }
    if (decoder_.error() != io::FrameError::kNone) {
      return Reply::kBroken;
    }
    const std::span<std::byte> space = decoder_.writable();
    asio::error_code error = asio::error::would_block;
    std::size_t read = 0;
    socket_.async_read_some(asio::buffer(space.data(), space.size()),
                            [&](const asio::error_code& result, std::size_t bytes) {
                              error = result;
                              read = bytes;
                            });
    io_.restart();
    io_.run_until(deadline);
    if (error == asio::error::would_block) {
      socket_.cancel();
      io_.restart();
      io_.run();
      return Reply::kTimeout;
    }
    if (error) {
      return Reply::kBroken;
    }
    decoder_.commit(read);
  }
}

std::optional<Client::Reply> Client::classify(const io::Frame& frame, std::uint64_t client,
                                              std::uint64_t seq, Ack& ack, std::uint32_t& leader) {
  if (frame.type == static_cast<std::uint16_t>(FrameType::kRedirect)) {
    const std::optional<Redirect> redirect = decode_redirect(frame.body);
    leader = redirect.value_or(Redirect{}).leader;
    return redirect ? Reply::kRedirect : Reply::kBroken;
  }
  std::optional<Ack> got = frame.type == static_cast<std::uint16_t>(FrameType::kAck)
                               ? decode_ack(frame.body)
                               : std::nullopt;
  if (!got) {
    return Reply::kBroken;
  }
  const bool registered =
      client == engine::kRegisterClient && got->result == engine::SessionResult::kRegistered;
  if (got->seq == seq && (registered || got->client == client)) {
    ack = std::move(*got);
    return Reply::kAck;
  }
  return std::nullopt;
}

void Client::move_on(std::optional<std::uint32_t> leader) {
  asio::error_code ignored;
  socket_.close(ignored);
  connected_ = false;
  target_ = leader && *leader <= options_.members.size() ? *leader - 1
                                                         : (target_ + 1) % options_.members.size();
}

}  // namespace rme::gateway
