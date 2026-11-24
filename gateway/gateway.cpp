#include "gateway/gateway.hpp"

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <utility>
#include <vector>

#include "io/frame.hpp"

namespace rme::gateway {

namespace {

constexpr auto kNoThrow = asio::as_tuple(asio::use_awaitable);

}  // namespace

struct Gateway::Connection {
  explicit Connection(asio::ip::tcp::socket s)
      : socket(std::move(s)), wake(socket.get_executor()) {}

  asio::ip::tcp::socket socket;
  asio::steady_timer wake;
  std::vector<std::byte> queued;
  std::vector<std::byte> writing;
  bool closed = false;
};

Gateway::Gateway(const std::string& host, std::uint16_t port)
    : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address(host), port)) {}

Gateway::~Gateway() {
  stop();
}

std::uint16_t Gateway::port() const {
  return acceptor_.local_endpoint().port();
}

node::Node::OutcomeHandler Gateway::outcome_handler() {
  return [this](std::uint64_t index, const engine::Outcome& outcome) {
    acks_.push(Ack{.index = index,
                   .client = outcome.client,
                   .seq = outcome.seq,
                   .result = outcome.result,
                   .events = outcome.events});
    if (!drain_posted_.exchange(true, std::memory_order_acq_rel)) {
      asio::post(io_, [this] { drain(); });
    }
  };
}

void Gateway::start(node::Node& node) {
  node_ = &node;
  asio::co_spawn(io_, accept(), asio::detached);
  thread_ = std::jthread([this] { io_.run(); });
}

void Gateway::stop() {
  io_.stop();
  if (thread_.joinable()) {
    thread_.join();
  }
}

asio::awaitable<void> Gateway::accept() {
  for (;;) {
    auto [error, socket] = co_await acceptor_.async_accept(kNoThrow);
    if (error == asio::error::operation_aborted) {
      co_return;
    }
    if (!error) {
      asio::error_code ignored;
      socket.set_option(asio::ip::tcp::no_delay(true), ignored);
      auto connection = std::make_shared<Connection>(std::move(socket));
      asio::co_spawn(io_, write_loop(connection), asio::detached);
      asio::co_spawn(io_, serve(std::move(connection)), asio::detached);
    }
  }
}

asio::awaitable<void> Gateway::serve(std::shared_ptr<Connection> connection) {
  io::FrameDecoder decoder(std::size_t{64} << 10U);
  while (!connection->closed) {
    const std::span<std::byte> space = decoder.writable();
    auto [error, read] = co_await connection->socket.async_read_some(
        asio::buffer(space.data(), space.size()), kNoThrow);
    if (error) {
      break;
    }
    decoder.commit(read);
    while (const std::optional<io::Frame> frame = decoder.next()) {
      if (frame->type != static_cast<std::uint16_t>(FrameType::kSubmit)) {
        connection->closed = true;
        break;
      }
      on_submit(connection, frame->body);
    }
    if (decoder.error() != io::FrameError::kNone) {
      break;
    }
  }
  connection->closed = true;
  connection->wake.cancel();
}

void Gateway::on_submit(const std::shared_ptr<Connection>& connection,
                        std::span<const std::byte> body) {
  const std::optional<engine::ClientCommand> command = decode_submit(body);
  if (!command) {
    connection->closed = true;
    return;
  }
  auto& route = command->client == engine::kRegisterClient ? registering_ : sessions_;
  const std::uint64_t key =
      command->client == engine::kRegisterClient ? command->seq : command->client;
  route[key] = connection;
  node_->submit({body.begin(), body.end()});
}

void Gateway::drain() {
  drain_posted_.store(false, std::memory_order_release);
  while (std::optional<Ack> ack = acks_.try_pop()) {
    std::shared_ptr<Connection> connection;
    if (ack->result == engine::SessionResult::kRegistered) {
      if (const auto found = registering_.find(ack->seq); found != registering_.end()) {
        connection = found->second.lock();
        registering_.erase(found);
        sessions_[ack->client] = connection;
      }
    } else if (const auto found = sessions_.find(ack->client); found != sessions_.end()) {
      connection = found->second.lock();
    }
    if (connection && !connection->closed) {
      encode_ack(*ack, connection->queued);
      connection->wake.cancel();
    }
  }
}

asio::awaitable<void> Gateway::write_loop(std::shared_ptr<Connection> connection) {
  while (!connection->closed) {
    if (connection->queued.empty()) {
      connection->wake.expires_at(asio::steady_timer::time_point::max());
      co_await connection->wake.async_wait(kNoThrow);
      continue;
    }
    std::swap(connection->queued, connection->writing);
    auto [error, written] =
        co_await asio::async_write(connection->socket, asio::buffer(connection->writing), kNoThrow);
    connection->writing.clear();
    if (error) {
      connection->closed = true;
    }
  }
  asio::error_code ignored;
  connection->socket.close(ignored);
}

}  // namespace rme::gateway
