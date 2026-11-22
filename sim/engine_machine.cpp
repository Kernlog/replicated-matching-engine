#include "sim/engine_machine.hpp"

#include <variant>

#include "codec/batch.hpp"
#include "core/assert.hpp"
#include "engine/book_hash.hpp"

namespace rme::sim {

namespace {

constexpr engine::BookConfig kBook{.max_orders = 1U << 12U, .price_levels = 1U << 14U};

}  // namespace

EngineMachine::EngineMachine() : book_(kBook) {
  scratch_.reserve(64);
}

void EngineMachine::apply(const consensus::Entry& entry) {
  RME_ASSERT(entry.index == applied_ + consensus::Index(1));
  applied_ = entry.index;
  if (entry.type != consensus::EntryType::kCommand) {
    return;
  }
  const bool decoded = codec::for_each_command(entry.data, [&](const engine::Command& command) {
    scratch_.clear();
    book_.apply(command, scratch_);
    ++commands_;
    for (const engine::Event& event : scratch_) {
      events_.add(event.index());
    }
  });
  RME_ASSERT(decoded);
}

std::uint64_t EngineMachine::book_hash() const {
  return engine::book_hash(book_);
}

}  // namespace rme::sim
