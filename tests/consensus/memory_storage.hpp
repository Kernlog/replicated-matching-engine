#pragma once

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

  void sync() {
    state_ = pending_state_;
    entries_ = pending_;
    ++syncs_;
  }

  [[nodiscard]] const HardState& state() const noexcept { return state_; }
  [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
  [[nodiscard]] int syncs() const noexcept { return syncs_; }

 private:
  HardState pending_state_;
  HardState state_;
  std::vector<Entry> pending_;
  std::vector<Entry> entries_;
  int syncs_ = 0;
};

}  // namespace rme::consensus
