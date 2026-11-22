#pragma once

#include <concepts>
#include <span>

#include "consensus/types.hpp"

namespace rme::consensus {

// append replaces every stored entry at or after entries.front().index. sync makes every earlier
// save and append durable together, so a crash never keeps entries newer than the saved term.
template <typename S>
concept Storage = requires(S& storage, const HardState& state, std::span<const Entry> entries) {
  storage.save(state);
  storage.append(entries);
  storage.sync();
};

template <typename T>
concept Transport = requires(T& transport, Message message) { transport.send(std::move(message)); };

// commit is volatile: after a restart, entries are applied again from index 1, so the machine
// must start empty.
template <typename M>
concept StateMachine = requires(M& machine, const Entry& entry) { machine.apply(entry); };

}  // namespace rme::consensus
