#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "consensus/raft.hpp"
#include "consensus/types.hpp"
#include "core/spsc.hpp"
#include "engine/replica.hpp"
#include "io/disk_storage.hpp"
#include "io/storage_thread.hpp"
#include "io/transport.hpp"

namespace rme::node {

struct NodeConfig {
  consensus::NodeId id;
  // Every member, this node included, in id order.
  std::vector<io::PeerAddress> members;
  std::filesystem::path data_dir;
  std::string listen_host = "0.0.0.0";
  std::chrono::milliseconds tick{10};
  consensus::RaftConfig raft;
  engine::ReplicaConfig replica;
};

// What the core thread last published, for health checks and tests.
struct NodeStatus {
  consensus::Role role = consensus::Role::kFollower;
  std::uint64_t term = 0;
  std::uint32_t leader = 0;
  std::uint64_t commit = 0;
  std::uint64_t applied = 0;
  std::uint64_t replica_hash = 0;
};

// One replica: a network thread (Asio), a core thread that owns Raft and the engine, and a storage
// thread that group-commits. The core never blocks on disk or network: it hands writes to the
// storage thread and holds every message and apply that depends on them until they are durable.
class Node {
 public:
  using OutcomeHandler = std::function<void(std::uint64_t index, const engine::Outcome&)>;

  Node(NodeConfig config, OutcomeHandler on_outcome = {});
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;
  Node(Node&&) = delete;
  Node& operator=(Node&&) = delete;
  ~Node();

  void start();
  // Stops the core first, then the network, then drains the storage thread.
  void stop();

  // From one client-facing thread: a command record for the leader to replicate.
  void submit(std::vector<std::byte> command) noexcept;

  [[nodiscard]] NodeStatus status() const noexcept;
  [[nodiscard]] std::uint16_t port() const { return transport_.port(); }

 private:
  struct Held {
    std::uint64_t seq = 0;
    std::optional<std::pair<consensus::Index, consensus::Term>> last;
    std::vector<consensus::Message> replies;
    std::optional<consensus::Snapshot> snapshot;
    std::vector<consensus::Entry> committed;
  };

  void run(const std::stop_token& stop);
  void handle_ready();
  bool release();
  void apply(const consensus::Entry& entry);
  void publish();
  io::StorageThread& storage_thread() noexcept;

  NodeConfig config_;
  OutcomeHandler on_outcome_;
  io::DiskStorage storage_;
  io::Durable recovered_;
  engine::Replica replica_;
  consensus::Raft raft_;
  SpscQueue<consensus::Message> inbox_{8192};
  SpscQueue<std::vector<std::byte>> commands_{8192};
  io::Transport transport_;
  std::optional<io::StorageThread> storage_thread_;
  std::deque<Held> held_;
  std::uint64_t seq_ = 0;
  bool sent_ = false;
  std::vector<engine::Outcome> outcomes_;
  struct alignas(kFalseSharingRange) Published {
    std::atomic<consensus::Role> role{consensus::Role::kFollower};
    std::atomic<std::uint32_t> leader{0};
    std::atomic<std::uint64_t> term{0};
    std::atomic<std::uint64_t> commit{0};
    std::atomic<std::uint64_t> applied{0};
    std::atomic<std::uint64_t> replica_hash{0};
  };
  Published published_;
  std::jthread core_;
};

}  // namespace rme::node
