#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "consensus/node.hpp"
#include "consensus/raft.hpp"
#include "core/hash.hpp"
#include "tests/consensus/memory_storage.hpp"

namespace rme::consensus {

struct ClusterOptions {
  std::uint64_t seed = 1;
  std::size_t max_entries = 64;
  std::size_t max_entry_bytes = 1;
  std::uint32_t max_inflight = 8;
  std::uint64_t max_uncommitted = 256;
  bool pre_vote = true;
  bool check_quorum = true;
};

// Base Raft as in the paper, without PreVote or CheckQuorum.
inline constexpr ClusterOptions kPaperRaft{.pre_vote = false, .check_quorum = false};

class Cluster {
 public:
  explicit Cluster(std::uint32_t size, ClusterOptions options = {})
      : storage_(size), applied_(size), group_(size, 0) {
    for (std::uint32_t i = 1; i <= size; ++i) {
      members_.emplace_back(i);
    }
    for (const NodeId id : members_) {
      configs_.push_back(RaftConfig{.id = id,
                                    .members = members_,
                                    .seed = options.seed * 1000 + id.value(),
                                    .max_entries_per_append = options.max_entries,
                                    .max_entry_bytes = options.max_entry_bytes,
                                    .max_inflight = options.max_inflight,
                                    .max_uncommitted_entries = options.max_uncommitted,
                                    .pre_vote = options.pre_vote,
                                    .check_quorum = options.check_quorum});
      nodes_.emplace_back(configs_.back());
    }
  }

  // Drops all volatile state and rebuilds the node from what it synced.
  void restart(std::uint32_t id) {
    const MemoryStorage& disk = storage_.at(id - 1);
    applied_.at(id - 1).restore(disk.snapshot());
    configs_.at(id - 1).seed += 7;
    nodes_.at(id - 1) = Raft(configs_.at(id - 1), disk.state(), disk.log(), disk.snapshot());
  }

  // Snapshots the node's state machine at its applied index and compacts its log behind it.
  void compact(std::uint32_t id) {
    consensus::compact(node(id), storage_.at(id - 1), applied_.at(id - 1).image());
  }

  // Digest of everything the node has applied, including what a snapshot stands in for.
  [[nodiscard]] std::uint64_t applied_digest(std::uint32_t id) const {
    return applied_.at(id - 1).digest.digest();
  }
  [[nodiscard]] std::uint64_t applied_count(std::uint32_t id) const {
    return applied_.at(id - 1).count;
  }

  // Replaces a node's durable state with the given term and entry terms, then restarts it.
  void load(std::uint32_t id, std::uint64_t term, const std::vector<std::uint64_t>& log_terms) {
    std::vector<Entry> entries;
    entries.reserve(log_terms.size());
    for (const std::uint64_t entry_term : log_terms) {
      entries.push_back(
          Entry{Term(entry_term), Index(entries.size() + 1), EntryType::kCommand, {}});
    }
    MemoryStorage& disk = storage_.at(id - 1);
    disk = MemoryStorage{};
    disk.save(HardState{Term(term), std::nullopt});
    if (!entries.empty()) {
      disk.append(entries);
    }
    disk.sync();
    restart(id);
  }

  [[nodiscard]] int rejections() const noexcept { return rejections_; }
  [[nodiscard]] std::size_t entries_sent() const noexcept { return entries_sent_; }

  // Drops every nth message in flight from now on; 0 disables loss.
  void drop_every(int nth) { drop_every_ = nth; }

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
    while (deliver_round()) {
    }
  }

  // Processes every node's Ready once and delivers that batch; replies wait for the next round.
  bool deliver_round() {
    Outbox outbox{this, {}};
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      process_ready(nodes_[i], storage_[i], outbox, applied_[i]);
    }
    const std::vector<Message> batch = std::move(outbox.messages);
    for (const Message& message : batch) {
      if (const auto* response = std::get_if<AppendResponse>(&message.body);
          response != nullptr && !response->success) {
        ++rejections_;
      }
      node(message.to.value()).step(message);
    }
    return !batch.empty() ||
           std::ranges::any_of(nodes_, [](const Raft& raft) { return raft.has_ready(); });
  }

  void start_campaign(std::uint32_t id) {
    while (node(id).role() == Role::kFollower) {
      node(id).tick();
    }
  }

  void campaign(std::uint32_t id) {
    start_campaign(id);
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

  void propose(std::uint32_t id, int count) {
    for (int i = 0; i < count; ++i) {
      const std::array payload{static_cast<std::byte>(next_payload_++)};
      node(id).propose(payload);
    }
    deliver();
  }

  [[nodiscard]] std::vector<Entry> log_of(std::uint32_t id) {
    const auto entries = node(id).log().slice(node(id).log().snapshot_index() + Index(1), SIZE_MAX);
    return {entries.begin(), entries.end()};
  }

  [[nodiscard]] bool logs_converged() {
    for (std::uint32_t id = 2; id <= nodes_.size(); ++id) {
      if (log_of(id) != log_of(1)) {
        return false;
      }
    }
    return true;
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
      if (const auto* append = std::get_if<AppendRequest>(&message.body)) {
        cluster->entries_sent_ += append->entries.size();
      }
      if (cluster->drop_every_ != 0 && ++cluster->sent_ % cluster->drop_every_ == 0) {
        return;
      }
      if (cluster->connected(message.from, message.to)) {
        messages.push_back(std::move(message));
      }
    }
  };

  struct Applied {
    std::vector<Entry> entries;
    Hasher digest;
    std::uint64_t count = 0;

    void apply(const Entry& entry) {
      entries.push_back(entry);
      digest.add(entry_digest(entry));
      ++count;
    }

    [[nodiscard]] std::vector<std::byte> image() const {
      std::vector<std::byte> out(2 * sizeof(std::uint64_t));
      const std::array words{digest.digest(), count};
      std::memcpy(out.data(), words.data(), out.size());
      return out;
    }

    void restore(const std::optional<Snapshot>& snapshot) {
      entries.clear();
      digest = Hasher{};
      count = 0;
      if (snapshot) {
        std::array<std::uint64_t, 2> words{};
        std::memcpy(words.data(), snapshot->data.data(), snapshot->data.size());
        digest = Hasher(words[0]);
        count = words[1];
      }
    }

    static std::uint64_t entry_digest(const Entry& entry) {
      Hasher hasher;
      hasher.add(entry.term.value());
      hasher.add(entry.index.value());
      hasher.add(entry.data.size());
      for (const std::byte byte : entry.data) {
        hasher.add(static_cast<std::uint64_t>(byte));
      }
      return hasher.digest();
    }
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
  unsigned next_payload_ = 0;
  int rejections_ = 0;
  std::size_t entries_sent_ = 0;
  int drop_every_ = 0;
  int sent_ = 0;
};

}  // namespace rme::consensus
