#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "consensus/node.hpp"
#include "consensus/raft.hpp"
#include "tests/consensus/memory_storage.hpp"

namespace rme::consensus {

class Cluster {
 public:
  explicit Cluster(std::uint32_t size, std::uint64_t seed = 1)
      : storage_(size), applied_(size), group_(size, 0) {
    for (std::uint32_t i = 1; i <= size; ++i) {
      members_.emplace_back(i);
    }
    for (const NodeId id : members_) {
      configs_.push_back(
          RaftConfig{.id = id, .members = members_, .seed = seed * 1000 + id.value()});
      nodes_.emplace_back(configs_.back());
    }
  }

  // Drops all volatile state and rebuilds the node from what it synced.
  void restart(std::uint32_t id) {
    const MemoryStorage& disk = storage_.at(id - 1);
    applied_.at(id - 1).entries.clear();
    configs_.at(id - 1).seed += 7;
    nodes_.at(id - 1) = Raft(configs_.at(id - 1), disk.state(), disk.entries());
  }

  [[nodiscard]] const MemoryStorage& storage(std::uint32_t id) const { return storage_.at(id - 1); }
  [[nodiscard]] const std::vector<Entry>& applied(std::uint32_t id) const {
    return applied_.at(id - 1).entries;
  }

  Raft& node(std::uint32_t id) { return nodes_.at(id - 1); }

  void tick_all() {
    for (Raft& raft : nodes_) {
      raft.tick();
    }
    deliver();
  }

  void deliver() {
    while (true) {
      Outbox outbox{this, {}};
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
        process_ready(nodes_[i], storage_[i], outbox, applied_[i]);
      }
      const std::vector<Message> batch = std::move(outbox.messages);
      if (batch.empty()) {
        return;
      }
      for (const Message& message : batch) {
        node(message.to.value()).step(message);
      }
    }
  }

  void campaign(std::uint32_t id) {
    while (node(id).role() == Role::kFollower) {
      node(id).tick();
    }
    deliver();
  }

  void partition(std::initializer_list<std::initializer_list<std::uint32_t>> groups) {
    int label = 1;
    for (const auto& group : groups) {
      for (const std::uint32_t id : group) {
        group_.at(id - 1) = label;
      }
      ++label;
    }
  }

  void isolate(std::uint32_t id) { group_.at(id - 1) = -static_cast<int>(id); }
  void heal() { std::ranges::fill(group_, 0); }

  std::optional<std::uint32_t> leader() {
    std::optional<std::uint32_t> found;
    Term best(0);
    for (const Raft& raft : nodes_) {
      if (raft.role() == Role::kLeader && raft.term() >= best) {
        best = raft.term();
        found = raft.id().value();
      }
    }
    return found;
  }

  std::uint32_t leader_id() { return leader().value_or(0); }

  template <typename Predicate>
  bool run_until(Predicate done, int max_ticks = 500) {
    for (int i = 0; i < max_ticks && !done(); ++i) {
      tick_all();
    }
    return done();
  }

 private:
  struct Outbox {
    Cluster* cluster;
    std::vector<Message> messages;

    void send(Message&& message) {
      if (cluster->connected(message.from, message.to)) {
        messages.push_back(std::move(message));
      }
    }
  };

  struct Applied {
    std::vector<Entry> entries;

    void apply(const Entry& entry) { entries.push_back(entry); }
  };

  [[nodiscard]] bool connected(NodeId from, NodeId to) const {
    return group_.at(from.value() - 1) == group_.at(to.value() - 1);
  }

  std::vector<NodeId> members_;
  std::vector<RaftConfig> configs_;
  std::vector<Raft> nodes_;
  std::vector<MemoryStorage> storage_;
  std::vector<Applied> applied_;
  std::vector<int> group_;
};

}  // namespace rme::consensus
