#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/spsc.hpp"
#include "md/feed.hpp"

namespace rme::md {

struct PublisherStats {
  std::uint64_t messages = 0;
  std::uint64_t packets = 0;
  std::uint64_t syscalls = 0;
  std::uint64_t dropped = 0;
};

// Sends the feed over UDP from its own thread. The core only splits entries and pushes messages;
// the publisher packs them into MTU-sized datagrams and hands up to 32 to one sendmmsg. When the
// ring is full the core drops instead of waiting, and subscribers recover the gap from a snapshot.
class Publisher {
 public:
  Publisher(const std::string& host, std::uint16_t port);
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;
  Publisher(Publisher&&) = delete;
  Publisher& operator=(Publisher&&) = delete;
  ~Publisher();

  // Core thread, once per applied entry, in index order; first_seq numbers its first message.
  void publish(std::uint64_t first_seq, std::uint64_t index,
               std::span<const std::byte> data) noexcept;

  // Valid after stop.
  [[nodiscard]] const PublisherStats& stats() const noexcept { return stats_; }
  void stop();

 private:
  void run(const std::stop_token& stop);
  void send(std::span<const Message> messages);

  int fd_ = -1;
  sockaddr_in destination_{};
  SpscQueue<Message> queue_{std::size_t{1} << 16U};
  std::vector<Message> split_;
  std::uint64_t dropped_ = 0;
  PublisherStats stats_;
  std::jthread thread_;
};

}  // namespace rme::md
