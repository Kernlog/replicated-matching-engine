#pragma once

#include <cstdint>
#include <vector>

#include "consensus/types.hpp"
#include "core/hash.hpp"
#include "engine/event.hpp"
#include "engine/order_book.hpp"

namespace rme::sim {

// The replicated state machine: every command entry is a batch of records applied to the book.
class EngineMachine {
 public:
  EngineMachine();

  void apply(const consensus::Entry& entry);

  [[nodiscard]] consensus::Index applied() const noexcept { return applied_; }
  [[nodiscard]] std::uint64_t book_hash() const;
  // Rolling hash of every engine output event, folded with the full book_hash every 64th index.
  [[nodiscard]] std::uint64_t state_hash() const;
  [[nodiscard]] std::uint64_t events_hash() const noexcept { return events_.digest(); }
  [[nodiscard]] std::uint64_t commands() const noexcept { return commands_; }

 private:
  engine::OrderBook book_;
  std::vector<engine::Event> scratch_;
  Hasher events_;
  consensus::Index applied_;
  std::uint64_t commands_ = 0;
};

}  // namespace rme::sim
