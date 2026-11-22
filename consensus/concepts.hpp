#pragma once

#include <concepts>
#include <span>

#include "consensus/types.hpp"

namespace rme::consensus {

// append may overwrite a suffix: entries.front().index can be at or below the stored last index.
template <typename S>
concept Storage = requires(S& storage, const HardState& state, std::span<const Entry> entries) {
  storage.save(state);
  storage.append(entries);
  storage.sync();
};

template <typename T>
concept Transport = requires(T& transport, Message message) { transport.send(std::move(message)); };

template <typename M>
concept StateMachine = requires(M& machine, const Entry& entry) { machine.apply(entry); };

}  // namespace rme::consensus
