#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "consensus/raft.hpp"

namespace rme::consensus {

class Cluster {
 public:
  explicit Cluster(std::uint32_t size, std::uint64_t seed = 1) : group_(size, 0) {
    for (std::uint32_t i = 1; i <= size; ++i) {
      members_.emplace_back(i);
    }
    for (const NodeId id : members_) {
      nodes_.emplace_back(
          RaftConfig{.id = id, .members = members_, .seed = seed * 1000 + id.value()});
    }
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
      std::vector<Message> batch;
      for (Raft& raft : nodes_) {
        if (!raft.has_ready()) {
          continue;
        }
        for (Message& message : raft.ready().messages) {
          if (connected(message.from, message.to)) {
            batch.push_back(std::move(message));
          }
        }
      }
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
  [[nodiscard]] bool connected(NodeId from, NodeId to) const {
    return group_.at(from.value() - 1) == group_.at(to.value() - 1);
  }

  std::vector<NodeId> members_;
  std::vector<Raft> nodes_;
  std::vector<int> group_;
};

}  // namespace rme::consensus
