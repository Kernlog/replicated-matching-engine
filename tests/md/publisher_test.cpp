#include "md/publisher.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace rme::md {
namespace {

class UdpSink {
 public:
  UdpSink() : fd_(::socket(AF_INET, SOCK_DGRAM, 0)) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto* raw = reinterpret_cast<sockaddr*>(&address);  // NOLINT(*-reinterpret-cast)
    socklen_t size = sizeof address;
    const int buffer = 16 << 20;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
    const timeval timeout{.tv_sec = 2, .tv_usec = 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    if (::bind(fd_, raw, sizeof address) == 0 && ::getsockname(fd_, raw, &size) == 0) {
      port_ = ntohs(address.sin_port);
    }
  }
  UdpSink(const UdpSink&) = delete;
  UdpSink& operator=(const UdpSink&) = delete;
  UdpSink(UdpSink&&) = delete;
  UdpSink& operator=(UdpSink&&) = delete;
  ~UdpSink() { ::close(fd_); }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  [[nodiscard]] std::optional<std::vector<Message>> next() const {
    std::array<std::byte, 2048> buffer{};
    const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n <= 0) {
      return std::nullopt;
    }
    EXPECT_LE(static_cast<std::size_t>(n), kMaxDatagram);
    return decode_packet(std::span(buffer).first(static_cast<std::size_t>(n)));
  }

 private:
  int fd_;
  std::uint16_t port_ = 0;
};

TEST(Publisher, SendsEveryRecordInSeqOrderInFullDatagrams) {
  UdpSink sink;
  constexpr std::uint64_t kEntries = 2000;
  std::uint64_t expected_messages = 0;
  PublisherStats stats;
  {
    Publisher publisher("127.0.0.1", sink.port());
    for (std::uint64_t index = 1; index <= kEntries; ++index) {
      const std::size_t records = index % 7;
      std::vector<std::byte> data(records * codec::kClientRecordSize,
                                  static_cast<std::byte>(index));
      publisher.publish(index, data);
      expected_messages += records == 0 ? 1 : records;
    }
    EXPECT_EQ(publisher.next_seq(), expected_messages + 1);
    publisher.stop();
    stats = publisher.stats();
  }
  std::uint64_t next = 1;
  std::uint64_t last_index = 0;
  while (next <= expected_messages) {
    const std::optional<std::vector<Message>> packet = sink.next();
    ASSERT_TRUE(packet.has_value()) << "stopped at seq " << next;
    for (const Message& message : packet.value_or(std::vector<Message>{})) {
      ASSERT_EQ(message.seq, next);
      ASSERT_GE(message.index, last_index);
      EXPECT_EQ(message.record[0],
                message.count == 0 ? std::byte{0} : static_cast<std::byte>(message.index));
      last_index = message.index;
      ++next;
    }
  }
  EXPECT_EQ(stats.messages, expected_messages);
  EXPECT_EQ(stats.dropped, 0U);
  EXPECT_LT(stats.syscalls * 4, stats.packets);
}

}  // namespace
}  // namespace rme::md
