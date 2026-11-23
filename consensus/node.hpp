#pragma once

#include <algorithm>
#include <utility>
#include <variant>

#include "consensus/concepts.hpp"
#include "consensus/raft.hpp"

namespace rme::consensus {

// A vote or an acknowledged entry must be durable before any message that depends on it leaves.
// The leader's AppendEntries depend on neither, so they go out before the local fsync and the
// write overlaps the network round trip (Raft thesis 10.2.1).
template <Storage S, Transport T, StateMachine M>
void process_ready(Raft& raft, S& storage, T& transport, M& machine) {
  if (!raft.has_ready()) {
    return;
  }
  Ready ready = raft.ready();
  const auto replies = std::ranges::stable_partition(ready.messages, [](const Message& message) {
    return std::holds_alternative<AppendRequest>(message.body);
  });
  for (auto it = ready.messages.begin(); it != replies.begin(); ++it) {
    transport.send(std::move(*it));
  }
  if (ready.hard_state) {
    storage.save(*ready.hard_state);
  }
  if (!ready.entries.empty()) {
    storage.append(ready.entries);
  }
  if (ready.hard_state || !ready.entries.empty()) {
    storage.sync();
  }
  if (!ready.entries.empty()) {
    raft.persisted(ready.entries.back().index);
  }
  for (Message& message : replies) {
    transport.send(std::move(message));
  }
  for (const Entry& entry : ready.committed) {
    machine.apply(entry);
  }
}

// Replaces the applied log prefix with the state machine image taken at raft.applied() and makes
// the snapshot durable.
template <Storage S>
void compact(Raft& raft, S& storage, std::vector<std::byte> image) {
  storage.save_snapshot(raft.compact(std::move(image)));
  storage.sync();
}

}  // namespace rme::consensus
