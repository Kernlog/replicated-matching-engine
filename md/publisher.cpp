#include "md/publisher.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace rme::md {

namespace {

constexpr std::size_t kPacketsPerSyscall = 32;
constexpr int kSendBuffer = 4 << 20;

}  // namespace

Publisher::Publisher(const std::string& host, std::uint16_t port)
    : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
  if (fd_ < 0) {
    throw std::system_error(errno, std::generic_category(), "socket");
  }
  destination_.sin_family = AF_INET;
  destination_.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &destination_.sin_addr) != 1) {
    ::close(fd_);
    throw std::invalid_argument("market data target must be an IPv4 address: " + host);
  }
  ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &kSendBuffer, sizeof kSendBuffer);
  split_.reserve(1024);
  thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

Publisher::~Publisher() {
  stop();
  ::close(fd_);
}

void Publisher::stop() {
  if (thread_.joinable()) {
    thread_.request_stop();
    queue_.push(Message{});
    thread_.join();
    stats_.dropped = dropped_;
  }
}

void Publisher::publish(std::uint64_t index, std::span<const std::byte> data) noexcept {
  split_.clear();
  split_entry(next_seq_, index, data, split_);
  next_seq_ += split_.size();
  const std::size_t pushed = queue_.push_n(split_);
  dropped_ += split_.size() - pushed;
}

void Publisher::run(const std::stop_token& stop) {
  std::vector<Message> batch(kMessagesPerPacket * kPacketsPerSyscall);
  for (;;) {
    queue_.wait();
    std::size_t count = queue_.pop_n(batch);
    const bool stopping = stop.stop_requested() && count != 0 && batch[count - 1].seq == 0;
    count -= stopping ? 1 : 0;
    std::size_t at = 0;
    // A drop leaves a hole in the seqs, and a packet only carries consecutive ones.
    while (at < count) {
      std::size_t end = at + 1;
      while (end < count && end - at < kMessagesPerPacket * kPacketsPerSyscall &&
             batch[end].seq == batch[end - 1].seq + 1) {
        ++end;
      }
      send(std::span(batch).subspan(at, end - at));
      at = end;
    }
    if (stopping) {
      return;
    }
  }
}

void Publisher::send(std::span<const Message> messages) {
  std::array<std::vector<std::byte>, kPacketsPerSyscall> packets;
  std::array<iovec, kPacketsPerSyscall> iov{};
  std::array<mmsghdr, kPacketsPerSyscall> headers{};
  std::size_t used = 0;
  while (!messages.empty()) {
    const std::size_t take = std::min(messages.size(), kMessagesPerPacket);
    packets[used].clear();
    encode_packet(messages.first(take), packets[used]);
    iov[used] = {packets[used].data(), packets[used].size()};
    headers[used].msg_hdr.msg_name = &destination_;
    headers[used].msg_hdr.msg_namelen = sizeof destination_;
    headers[used].msg_hdr.msg_iov = &iov[used];
    headers[used].msg_hdr.msg_iovlen = 1;
    stats_.messages += take;
    messages = messages.subspan(take);
    ++used;
  }
  std::size_t sent = 0;
  while (sent < used) {
    const int n = ::sendmmsg(fd_, &headers[sent], static_cast<unsigned>(used - sent), 0);
    ++stats_.syscalls;
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    sent += static_cast<std::size_t>(n);
  }
  stats_.packets += sent;
}

}  // namespace rme::md
