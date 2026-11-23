#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "core/strong.hpp"

namespace rme::consensus {

struct TermTag {
  static constexpr bool kAdditive = true;
};
struct IndexTag {
  static constexpr bool kAdditive = true;
};
struct NodeIdTag {
  static constexpr bool kAdditive = false;
};

using Term = Strong<std::uint64_t, TermTag>;
using Index = Strong<std::uint64_t, IndexTag>;
using NodeId = Strong<std::uint32_t, NodeIdTag>;

enum class EntryType : std::uint8_t { kNoop, kCommand };

struct Entry {
  Term term;
  Index index;
  EntryType type = EntryType::kCommand;
  std::vector<std::byte> data;

  friend bool operator==(const Entry&, const Entry&) = default;
};

struct HardState {
  Term term;
  std::optional<NodeId> vote;

  friend bool operator==(const HardState&, const HardState&) = default;
};

// State machine image at index (term is the log term there); the log keeps only what follows.
struct Snapshot {
  Index index;
  Term term;
  std::vector<std::byte> data;

  friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

struct VoteRequest {
  Index last_index;
  Term last_term;
  bool pre_vote = false;

  friend bool operator==(const VoteRequest&, const VoteRequest&) = default;
};

struct VoteResponse {
  bool granted = false;
  bool pre_vote = false;

  friend bool operator==(const VoteResponse&, const VoteResponse&) = default;
};

struct AppendRequest {
  Index prev_index;
  Term prev_term;
  std::vector<Entry> entries;
  Index commit;

  friend bool operator==(const AppendRequest&, const AppendRequest&) = default;
};

struct AppendResponse {
  bool success = false;
  Index match;
  Index conflict_index;
  Term conflict_term;

  friend bool operator==(const AppendResponse&, const AppendResponse&) = default;
};

using Body = std::variant<VoteRequest, VoteResponse, AppendRequest, AppendResponse>;

struct Message {
  NodeId from;
  NodeId to;
  Term term;
  Body body;

  friend bool operator==(const Message&, const Message&) = default;
};

// Persist hard_state and entries, then send messages, then apply committed entries.
struct Ready {
  std::optional<HardState> hard_state;
  std::vector<Entry> entries;
  std::vector<Message> messages;
  std::vector<Entry> committed;

  [[nodiscard]] bool empty() const noexcept {
    return !hard_state && entries.empty() && messages.empty() && committed.empty();
  }
};

}  // namespace rme::consensus
