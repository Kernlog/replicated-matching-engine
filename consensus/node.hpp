#pragma once

#include <utility>

#include "consensus/concepts.hpp"
#include "consensus/raft.hpp"

namespace rme::consensus {

// A vote or an acknowledged entry must be durable before any message that depends on it leaves.
template <Storage S, Transport T, StateMachine M>
void process_ready(Raft& raft, S& storage, T& transport, M& machine) {
  if (!raft.has_ready()) {
    return;
  }
  Ready ready = raft.ready();
  if (ready.hard_state) {
    storage.save(*ready.hard_state);
  }
  if (!ready.entries.empty()) {
    storage.append(ready.entries);
  }
  if (ready.hard_state || !ready.entries.empty()) {
    storage.sync();
  }
  for (Message& message : ready.messages) {
    transport.send(std::move(message));
  }
  for (const Entry& entry : ready.committed) {
    machine.apply(entry);
  }
}

}  // namespace rme::consensus
