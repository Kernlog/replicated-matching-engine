#include "io/transport.hpp"

#include <algorithm>
#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <chrono>
#include <utility>

#include "io/frame.hpp"
#include "io/raft_codec.hpp"

namespace rme::io {

namespace {

constexpr auto kNoThrow = asio::as_tuple(asio::use_awaitable);
constexpr int kSocketBuffer = 4 << 20;
constexpr auto kFirstRetry = std::chrono::milliseconds(20);
constexpr auto kMaxRetry = std::chrono::milliseconds(500);
constexpr std::size_t kMaxQueuedFrames = 4096;

void configure(asio::ip::tcp::socket& socket) {
  asio::error_code ignored;
  socket.set_option(asio::ip::tcp::no_delay(true), ignored);
  socket.set_option(asio::socket_base::send_buffer_size(kSocketBuffer), ignored);
  socket.set_option(asio::socket_base::receive_buffer_size(kSocketBuffer), ignored);
}

}  // namespace

struct Transport::Peer {
  Peer(asio::io_context& io, PeerAddress where) : address(std::move(where)), socket(io), wake(io) {}

  PeerAddress address;
  asio::ip::tcp::socket socket;
  asio::steady_timer wake;
  std::vector<std::vector<std::byte>> queued;
  std::vector<std::vector<std::byte>> writing;
  std::vector<std::vector<std::byte>> spare;
  std::vector<asio::const_buffer> buffers;
  bool connected = false;
};

Transport::Transport(consensus::NodeId self, const std::string& host, std::uint16_t port,
                     Handler on_message)
    : self_(self),
      on_message_(std::move(on_message)),
      acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address(host), port)) {}

Transport::~Transport() {
  stop();
}

std::uint16_t Transport::port() const {
  return acceptor_.local_endpoint().port();
}

void Transport::add_peer(PeerAddress address) {
  const consensus::NodeId id = address.id;
  peers_.emplace(id, std::make_unique<Peer>(io_, std::move(address)));
}

void Transport::start() {
  asio::co_spawn(io_, accept(), asio::detached);
  for (auto& [id, peer] : peers_) {
    asio::co_spawn(io_, dial(peer.get()), asio::detached);
  }
  thread_ = std::jthread([this] { io_.run(); });
}

void Transport::stop() {
  io_.stop();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Transport::send(consensus::Message&& message) noexcept {
  outbox_.push(std::move(message));
}

void Transport::flush() {
  if (!drain_posted_.exchange(true, std::memory_order_acq_rel)) {
    asio::post(io_, [this] { drain(); });
  }
}

void Transport::drain() {
  drain_posted_.store(false, std::memory_order_release);
  while (std::optional<consensus::Message> message = outbox_.try_pop()) {
    const auto found = peers_.find(message->to);
    if (found == peers_.end() || !found->second->connected ||
        found->second->queued.size() >= kMaxQueuedFrames) {
      ++stats_.dropped;
      continue;
    }
    Peer& peer = *found->second;
    std::vector<std::byte> frame;
    if (!peer.spare.empty()) {
      frame = std::move(peer.spare.back());
      peer.spare.pop_back();
    }
    frame.clear();
    scratch_.clear();
    encode_message(*message, scratch_);
    encode_frame(kRaftFrame, scratch_, frame);
    peer.queued.push_back(std::move(frame));
    peer.wake.cancel();
  }
}

asio::awaitable<void> Transport::accept() {
  for (;;) {
    auto [error, socket] = co_await acceptor_.async_accept(kNoThrow);
    if (error == asio::error::operation_aborted) {
      co_return;
    }
    if (!error) {
      asio::co_spawn(io_, serve(std::move(socket)), asio::detached);
    }
  }
}

asio::awaitable<void> Transport::serve(asio::ip::tcp::socket socket) {
  configure(socket);
  FrameDecoder decoder;
  for (;;) {
    const std::span<std::byte> space = decoder.writable();
    auto [error, read] =
        co_await socket.async_read_some(asio::buffer(space.data(), space.size()), kNoThrow);
    if (error) {
      co_return;
    }
    decoder.commit(read);
    while (const std::optional<Frame> frame = decoder.next()) {
      std::optional<consensus::Message> message =
          frame->type == kRaftFrame ? decode_message(frame->body) : std::nullopt;
      if (!message || message->to != self_) {
        co_return;
      }
      on_message_(std::move(*message));
    }
    if (decoder.error() != FrameError::kNone) {
      co_return;
    }
  }
}

asio::awaitable<void> Transport::dial(Peer* peer) {
  asio::ip::tcp::resolver resolver(io_);
  auto delay = kFirstRetry;
  for (;;) {
    auto [resolve_error, endpoints] = co_await resolver.async_resolve(
        peer->address.host, std::to_string(peer->address.port), kNoThrow);
    if (!resolve_error) {
      auto [connect_error, endpoint] =
          co_await asio::async_connect(peer->socket, endpoints, kNoThrow);
      if (!connect_error) {
        configure(peer->socket);
        peer->connected = true;
        ++stats_.connects;
        delay = kFirstRetry;
        co_await write_loop(peer);
        peer->connected = false;
        stats_.dropped += peer->queued.size();
        peer->queued.clear();
      }
    }
    asio::error_code ignored;
    peer->socket.close(ignored);
    peer->wake.expires_after(delay);
    co_await peer->wake.async_wait(kNoThrow);
    delay = std::min<std::chrono::milliseconds>(delay * 2, kMaxRetry);
  }
}

asio::awaitable<void> Transport::write_loop(Peer* peer) {
  for (;;) {
    while (peer->queued.empty()) {
      peer->wake.expires_at(asio::steady_timer::time_point::max());
      co_await peer->wake.async_wait(kNoThrow);
    }
    std::swap(peer->queued, peer->writing);
    peer->buffers.clear();
    for (const std::vector<std::byte>& frame : peer->writing) {
      peer->buffers.emplace_back(frame.data(), frame.size());
    }
    auto [error, written] = co_await asio::async_write(peer->socket, peer->buffers, kNoThrow);
    ++stats_.writes;
    stats_.frames_sent += error ? 0 : peer->writing.size();
    for (std::vector<std::byte>& frame : peer->writing) {
      peer->spare.push_back(std::move(frame));
    }
    peer->writing.clear();
    if (error) {
      co_return;
    }
  }
}

}  // namespace rme::io
