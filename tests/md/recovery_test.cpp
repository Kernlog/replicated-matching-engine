#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>

#include "core/rng.hpp"
#include "gateway/client.hpp"
#include "md/publisher.hpp"
#include "md/subscriber.hpp"
#include "testing/command_stream.hpp"
#include "tests/node/cluster.hpp"

namespace rme::md {
namespace {

using node::LocalCluster;

// Forwards datagrams from the publisher to the subscriber, dropping a fixed share of them.
class LossyRelay {
 public:
  LossyRelay(std::uint16_t to, std::uint64_t drop_percent) : in_(::socket(AF_INET, SOCK_DGRAM, 0)) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto* raw = reinterpret_cast<sockaddr*>(&address);  // NOLINT(*-reinterpret-cast)
    socklen_t size = sizeof address;
    const int buffer = 16 << 20;
    ::setsockopt(in_, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
    const timeval wait{.tv_sec = 0, .tv_usec = 20'000};
    ::setsockopt(in_, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
    EXPECT_EQ(::bind(in_, raw, sizeof address), 0);
    EXPECT_EQ(::getsockname(in_, raw, &size), 0);
    port_ = ntohs(address.sin_port);
    address.sin_port = htons(to);
    thread_ = std::jthread([this, address, drop_percent](const std::stop_token& stop) {
      Rng rng(5);
      std::array<std::byte, 2048> datagram{};
      while (!stop.stop_requested()) {
        const ssize_t n = ::recv(in_, datagram.data(), datagram.size(), 0);
        if (n <= 0) {
          continue;
        }
        ++received_;
        if (rng.below(100) < drop_percent) {
          ++dropped_;
          continue;
        }
        ::sendto(in_, datagram.data(), static_cast<std::size_t>(n), 0,
                 reinterpret_cast<const sockaddr*>(&address),  // NOLINT(*-reinterpret-cast)
                 sizeof address);
      }
    });
  }
  LossyRelay(const LossyRelay&) = delete;
  LossyRelay& operator=(const LossyRelay&) = delete;
  LossyRelay(LossyRelay&&) = delete;
  LossyRelay& operator=(LossyRelay&&) = delete;
  ~LossyRelay() {
    thread_.request_stop();
    thread_.join();
    ::close(in_);
  }

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(); }
  [[nodiscard]] std::uint64_t received() const noexcept { return received_.load(); }

 private:
  int in_;
  std::uint16_t port_ = 0;
  std::atomic<std::uint64_t> received_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::jthread thread_;
};

TEST(Recovery, SubscriberRebuildsTheLeadersBookThroughFivePercentLoss) {
  std::unique_ptr<Subscriber> subscriber;
  std::unique_ptr<LossyRelay> relay;
  std::unique_ptr<Publisher> publisher;
  LocalCluster cluster(3, [&](std::uint32_t id, node::Node& node, gateway::Gateway& gateway) {
    if (id != 1) {
      return;
    }
    subscriber = std::make_unique<Subscriber>(0, gateway::Endpoint{"127.0.0.1", gateway.port()});
    relay = std::make_unique<LossyRelay>(subscriber->port(), 5);
    publisher = std::make_unique<Publisher>("127.0.0.1", relay->port());
    node.on_entry([&publisher](const consensus::Entry& entry) {
      publisher->publish(entry.index.value(), entry.data);
    });
    gateway.feed_seq([&publisher] { return publisher->next_seq(); });
  });
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  gateway::ClientOptions options;
  for (std::uint32_t id = 1; id <= 3; ++id) {
    options.members.push_back({"127.0.0.1", cluster.client_port(id)});
  }
  gateway::Client client(options);
  client.open();
  testing::CommandStream stream(17);
  for (int i = 0; i < 1500; ++i) {
    client.submit(stream.next());
  }
  ASSERT_TRUE(LocalCluster::eventually([&] {
    const node::NodeStatus status = cluster.node(1).status();
    return status.applied != 0 && status.applied == status.commit;
  }));
  const std::uint64_t target = cluster.node(1).status().applied;
  ASSERT_TRUE(subscriber->run_until(target, std::chrono::seconds(20)));
  ASSERT_TRUE(LocalCluster::eventually(
      [&] { return cluster.node(1).status().replica_hash == subscriber->hash(); }));
  EXPECT_GT(relay->dropped(), 0U);
  EXPECT_GE(subscriber->stats().recoveries, 1U);
  std::cout << "packets " << relay->received() << " dropped " << relay->dropped() << " recoveries "
            << subscriber->stats().recoveries << " applied " << target << '\n';
}

}  // namespace
}  // namespace rme::md
