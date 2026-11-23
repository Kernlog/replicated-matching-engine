#include "sim/engine_machine.hpp"

#include <optional>
#include <variant>

#include "core/assert.hpp"
#include "engine/book_hash.hpp"

namespace rme::sim {

namespace {

engine::ReplicaConfig replica_config() {
  return {.book = engine::BookConfig{.max_orders = 1U << 12U, .price_levels = 1U << 14U},
          .session_ttl_entries = 1U << 14U};
}

}  // namespace

EngineMachine::EngineMachine() : replica_(replica_config()) {
  outcomes_.reserve(64);
}

void EngineMachine::apply(const consensus::Entry& entry) {
  RME_ASSERT(entry.index == applied_ + consensus::Index(1));
  applied_ = entry.index;
  outcomes_.clear();
  replica_.apply(entry.index.value(), entry.data, outcomes_);
  Hasher digest;
  for (const engine::Outcome& outcome : outcomes_) {
    digest.add(outcome.client);
    digest.add(outcome.seq);
    digest.add(static_cast<std::uint64_t>(outcome.result));
    digest.add(outcome.events.size());
    commands_ += outcome.result == engine::SessionResult::kApplied ? 1U : 0U;
  }
  entry_hash_ = digest.digest();
  outcomes_hash_.add(entry_hash_);
}

void EngineMachine::restore(const consensus::Snapshot& snapshot) {
  std::optional<engine::Replica> restored =
      engine::Replica::restore(snapshot.data, replica_config());
  RME_ASSERT(restored.has_value());
  replica_ = std::move(*restored);
  applied_ = snapshot.index;
  outcomes_.clear();
  entry_hash_ = 0;
}

std::uint64_t EngineMachine::book_hash() const {
  return engine::book_hash(replica_.book());
}

std::uint64_t EngineMachine::state_hash() const {
  Hasher hasher;
  hasher.add(entry_hash_);
  if (applied_.value() % 64 == 0) {
    hasher.add(replica_.hash());
  }
  return hasher.digest();
}

}  // namespace rme::sim
