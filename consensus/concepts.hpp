#pragma once

#include <concepts>
#include <span>

#include "consensus/types.hpp"

namespace rme::consensus {

// append replaces every stored entry at or after entries.front().index. save_snapshot replaces
// everything up to the snapshot index, and later entries too unless the stored log agrees with the
// snapshot's term there. sync makes every earlier save, save_snapshot and append durable together,
// so a crash never keeps entries newer than the saved term.
template <typename S>
concept Storage = requires(S& storage, const HardState& state, const Snapshot& snapshot,
                           std::span<const Entry> entries) {
  storage.save(state);
  storage.save_snapshot(snapshot);
  storage.append(entries);
  storage.sync();
};

template <typename T>
concept Transport = requires(T& transport, Message message) { transport.send(std::move(message)); };

// commit is volatile: after a restart, entries are applied again from the snapshot index (or 1),
// so the machine must start from that snapshot (or empty). restore replaces the whole state with a
// snapshot a leader installed.
template <typename M>
concept StateMachine = requires(M& machine, const Entry& entry, const Snapshot& snapshot) {
  machine.apply(entry);
  machine.restore(snapshot);
};

}  // namespace rme::consensus
