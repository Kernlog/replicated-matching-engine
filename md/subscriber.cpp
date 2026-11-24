#include "md/subscriber.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <asio/connect.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <cerrno>
#include <stdexcept>
#include <system_error>

#include "gateway/protocol.hpp"
#include "io/frame.hpp"

namespace rme::md {

namespace {

// Loopback and LAN multicast rarely reorder, so a hole this many messages old is a drop.
constexpr std::size_t kReorderWindow = 256;

}  // namespace

Subscriber::Subscriber(std::uint16_t port, gateway::Endpoint recovery, engine::ReplicaConfig config)
    : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)),
      recovery_(std::move(recovery)),
      config_(config),
      replica_(config) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  auto* raw = reinterpret_cast<sockaddr*>(&address);  // NOLINT(*-reinterpret-cast)
  socklen_t size = sizeof address;
  const int buffer = 16 << 20;
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
  if (::bind(fd_, raw, sizeof address) != 0 || ::getsockname(fd_, raw, &size) != 0) {
    const int error = errno;
    ::close(fd_);
    throw std::system_error(error, std::generic_category(), "bind feed socket");
  }
  port_ = ntohs(address.sin_port);
  outcomes_.reserve(64);
}

Subscriber::~Subscriber() {
  ::close(fd_);
}

bool Subscriber::run_until(std::uint64_t index, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::byte, 2048> buffer{};
  const timeval wait{.tv_sec = 0, .tv_usec = 20'000};
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
  while (applied() < index && std::chrono::steady_clock::now() < deadline) {
    const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n > 0) {
      on_datagram(std::span(buffer).first(static_cast<std::size_t>(n)));
    } else if (!buffered_.empty()) {
      recover();
    }
  }
  return applied() >= index;
}

void Subscriber::on_datagram(std::span<const std::byte> bytes) {
  ++stats_.packets;
  const std::optional<std::vector<Message>> messages = decode_packet(bytes);
  if (!messages) {
    ++stats_.bad_packets;
    return;
  }
  for (const Message& message : *messages) {
    if (message.seq >= next_seq_) {
      buffered_.emplace(message.seq, message);
    }
  }
  drain();
  if (!buffered_.empty() && buffered_.rbegin()->first - next_seq_ >= kReorderWindow) {
    recover();
  }
}

void Subscriber::drain() {
  while (!buffered_.empty() && buffered_.begin()->first == next_seq_) {
    deliver(buffered_.begin()->second);
    buffered_.erase(buffered_.begin());
    ++next_seq_;
  }
}

void Subscriber::deliver(const Message& message) {
  if (message.position == 0) {
    assembling_.clear();
  }
  if (message.count != 0) {
    assembling_.insert(assembling_.end(), message.record.begin(), message.record.end());
  }
  if (message.count == 0 || message.position + 1 == message.count) {
    outcomes_.clear();
    replica_.apply(message.index, assembling_, outcomes_);
    assembling_.clear();
  }
}

void Subscriber::recover() {
  ++stats_.recoveries;
  asio::io_context io;
  asio::ip::tcp::socket socket(io);
  asio::ip::tcp::resolver resolver(io);
  asio::connect(socket, resolver.resolve(recovery_.host, std::to_string(recovery_.port)));
  std::vector<std::byte> request;
  io::encode_frame(static_cast<std::uint16_t>(gateway::FrameType::kSnapshotRequest), {}, request);
  asio::write(socket, asio::buffer(request));
  io::FrameDecoder decoder;
  for (;;) {
    if (const std::optional<io::Frame> frame = decoder.next()) {
      std::optional<gateway::FeedSnapshot> snapshot =
          frame->type == static_cast<std::uint16_t>(gateway::FrameType::kSnapshot)
              ? gateway::decode_snapshot(frame->body)
              : std::nullopt;
      std::optional<engine::Replica> replica =
          snapshot ? engine::Replica::restore(snapshot->image, config_) : std::nullopt;
      if (!replica) {
        throw std::runtime_error("bad snapshot from " + recovery_.host);
      }
      replica_ = std::move(*replica);
      next_seq_ = snapshot.value_or(gateway::FeedSnapshot{}).next_seq;
      assembling_.clear();
      buffered_.erase(buffered_.begin(), buffered_.lower_bound(next_seq_));
      drain();
      return;
    }
    const std::span<std::byte> space = decoder.writable();
    decoder.commit(socket.read_some(asio::buffer(space.data(), space.size())));
  }
}

}  // namespace rme::md
