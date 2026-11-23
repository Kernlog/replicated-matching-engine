#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "consensus/types.hpp"
#include "core/hash.hpp"
#include "engine/replica.hpp"

namespace rme::sim {

// The replicated state machine: every entry is a batch of client records applied to the replica
// (book plus sessions). Outcomes of the latest entry stay readable for the client model.
class EngineMachine {
 public:
  EngineMachine();

  void apply(const consensus::Entry& entry);
  void restore(const consensus::Snapshot& snapshot);
  [[nodiscard]] std::vector<std::byte> snapshot() const { return replica_.snapshot(); }

  [[nodiscard]] consensus::Index applied() const noexcept { return applied_; }
  [[nodiscard]] std::uint64_t book_hash() const;
  // Hash of the latest entry's outcomes, folded with the full replica hash every 64th index; equal
  // on every replica at the same index whether it got there by applying or by a snapshot.
  [[nodiscard]] std::uint64_t state_hash() const;
  [[nodiscard]] std::uint64_t events_hash() const noexcept { return outcomes_hash_.digest(); }
  [[nodiscard]] std::uint64_t commands() const noexcept { return commands_; }
  [[nodiscard]] std::span<const engine::Outcome> last_outcomes() const noexcept {
    return outcomes_;
  }

 private:
  engine::Replica replica_;
  std::vector<engine::Outcome> outcomes_;
  Hasher outcomes_hash_;
  std::uint64_t entry_hash_ = 0;
  consensus::Index applied_;
  std::uint64_t commands_ = 0;
};

}  // namespace rme::sim
