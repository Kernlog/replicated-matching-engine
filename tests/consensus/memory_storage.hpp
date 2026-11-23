#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "consensus/types.hpp"

namespace rme::consensus {

class MemoryStorage {
 public:
  void save(const HardState& state) { pending_state_ = state; }

  void append(std::span<const Entry> entries) {
    pending_.resize(entries.front().index.value() - 1, Entry{});
    pending_.insert(pending_.end(), entries.begin(), entries.end());
  }

  void save_snapshot(const Snapshot& snapshot) {
    const std::size_t at = snapshot.index.value();
    if (pending_.size() < at || pending_[at - 1].term != snapshot.term) {
      pending_.resize(at, Entry{});
    }
    pending_snapshot_ = snapshot;
  }

  void sync() {
    state_ = pending_state_;
    entries_ = pending_;
    snapshot_ = pending_snapshot_;
    ++syncs_;
  }

  [[nodiscard]] const std::optional<Snapshot>& snapshot() const noexcept { return snapshot_; }
  // The durable entries after the snapshot point, as a restart reads them.
  [[nodiscard]] std::vector<Entry> log() const {
    const std::size_t from = snapshot_ ? snapshot_->index.value() : 0;
    return {entries_.begin() + static_cast<std::ptrdiff_t>(std::min(from, entries_.size())),
            entries_.end()};
  }

  [[nodiscard]] const HardState& state() const noexcept { return state_; }
  [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
  [[nodiscard]] int syncs() const noexcept { return syncs_; }

 private:
  HardState pending_state_;
  HardState state_;
  std::vector<Entry> pending_;
  std::vector<Entry> entries_;
  std::optional<Snapshot> pending_snapshot_;
  std::optional<Snapshot> snapshot_;
  int syncs_ = 0;
};

}  // namespace rme::consensus
