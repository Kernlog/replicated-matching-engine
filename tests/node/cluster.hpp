#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "gateway/gateway.hpp"
#include "node/node.hpp"
#include "tests/io/temp_dir.hpp"

namespace rme::node {

// A port the kernel just handed out and released, so a node can bind it right after.
inline std::uint16_t free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  auto* raw = reinterpret_cast<sockaddr*>(&address);  // NOLINT(*-reinterpret-cast)
  socklen_t size = sizeof address;
  const bool bound = ::bind(fd, raw, sizeof address) == 0 && ::getsockname(fd, raw, &size) == 0;
  ::close(fd);
  return bound ? ntohs(address.sin_port) : 0;
}

struct Recorded {
  std::uint64_t index = 0;
  engine::Outcome outcome;
};

class LocalCluster {
 public:
  using Configure = std::function<void(std::uint32_t id, Node&, gateway::Gateway&)>;

  // configure runs for each node before it starts, to attach entry hooks.
  explicit LocalCluster(std::uint32_t size, Configure configure = {},
                        consensus::RaftConfig raft = {})
      : configure_(std::move(configure)),
        raft_(std::move(raft)),
        dirs_(size),
        outcomes_(size),
        gateways_(size),
        nodes_(size) {
    // A port handed out and released can be taken by a test running in parallel before a node
    // binds it, so a clash picks fresh ports and starts over.
    for (int attempt = 0;; ++attempt) {
      members_.clear();
      client_ports_.clear();
      for (std::uint32_t id = 1; id <= size; ++id) {
        members_.push_back({consensus::NodeId(id), "127.0.0.1", free_port()});
        client_ports_.push_back(free_port());
      }
      try {
        for (std::uint32_t id = 1; id <= size; ++id) {
          start(id);
        }
        return;
      } catch (const std::system_error& error) {
        if (error.code() != std::errc::address_in_use || attempt == 9) {
          throw;
        }
        for (std::uint32_t id = 1; id <= size; ++id) {
          stop(id);
        }
      }
    }
  }

  void start(std::uint32_t id) {
    NodeConfig config;
    config.id = consensus::NodeId(id);
    config.members = members_;
    config.data_dir = dirs_[id - 1].path();
    config.listen_host = "127.0.0.1";
    config.tick = std::chrono::milliseconds(5);
    config.raft = raft_;
    auto& sink = outcomes_[id - 1];
    gateways_[id - 1] = std::make_unique<gateway::Gateway>("127.0.0.1", client_ports_[id - 1]);
    nodes_[id - 1] =
        std::make_unique<Node>(config, [&sink, forward = gateways_[id - 1]->outcome_handler()](
                                           std::uint64_t index, const engine::Outcome& outcome) {
          forward(index, outcome);
          const std::scoped_lock lock(sink.mutex);
          sink.recorded.push_back({index, outcome});
        });
    if (configure_) {
      configure_(id, *nodes_[id - 1], *gateways_[id - 1]);
    }
    nodes_[id - 1]->start();
    gateways_[id - 1]->start(*nodes_[id - 1]);
  }

  void stop(std::uint32_t id) {
    gateways_[id - 1].reset();
    nodes_[id - 1].reset();
  }

  [[nodiscard]] std::uint16_t client_port(std::uint32_t id) const { return client_ports_[id - 1]; }

  Node& node(std::uint32_t id) { return *nodes_[id - 1]; }
  [[nodiscard]] bool running(std::uint32_t id) const { return nodes_[id - 1] != nullptr; }

  std::vector<Recorded> outcomes(std::uint32_t id) {
    const std::scoped_lock lock(outcomes_[id - 1].mutex);
    return outcomes_[id - 1].recorded;
  }

  std::optional<std::uint32_t> leader() {
    for (std::uint32_t id = 1; id <= nodes_.size(); ++id) {
      if (running(id) && node(id).status().role == consensus::Role::kLeader) {
        return id;
      }
    }
    return std::nullopt;
  }

  template <typename Predicate>
  static bool eventually(Predicate done, std::chrono::seconds timeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (done()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
  }

 private:
  struct Sink {
    std::mutex mutex;
    std::vector<Recorded> recorded;
  };

  Configure configure_;
  consensus::RaftConfig raft_;
  std::vector<io::PeerAddress> members_;
  std::vector<std::uint16_t> client_ports_;
  std::vector<io::TempDir> dirs_;
  std::vector<Sink> outcomes_;
  std::vector<std::unique_ptr<gateway::Gateway>> gateways_;
  std::vector<std::unique_ptr<Node>> nodes_;
};

}  // namespace rme::node
