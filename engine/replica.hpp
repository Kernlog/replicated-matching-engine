#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "engine/client_command.hpp"
#include "engine/event.hpp"
#include "engine/order_book.hpp"

namespace rme::engine {

enum class SessionResult : std::uint8_t {
  kRegistered,
  kApplied,
  kDuplicate,
  kStale,
  kUnknownSession,
  kOutOfOrder,
};

struct Outcome {
  std::uint64_t client = 0;
  std::uint64_t seq = 0;
  SessionResult result = SessionResult::kUnknownSession;
  std::vector<Event> events;

  friend bool operator==(const Outcome&, const Outcome&) = default;
};

struct ReplicaConfig {
  BookConfig book;
  std::uint64_t session_ttl_entries = 1U << 20U;
};

struct Session {
  std::uint64_t last_seq = 0;
  std::vector<Event> events;

  friend bool operator==(const Session&, const Session&) = default;
};

class Replica {
 public:
  explicit Replica(ReplicaConfig config = {});

  void apply(std::uint64_t log_index, std::span<const std::byte> batch, std::vector<Outcome>& out);

  [[nodiscard]] std::uint64_t hash() const;
  [[nodiscard]] std::vector<std::byte> snapshot() const;
  [[nodiscard]] static std::optional<Replica> restore(std::span<const std::byte> bytes,
                                                      ReplicaConfig config = {});

  [[nodiscard]] std::uint64_t applied_index() const noexcept { return applied_index_; }
  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

 private:
  Replica(ReplicaConfig config, OrderBook book);

  [[nodiscard]] Outcome open_session(std::uint64_t log_index, std::uint64_t position);
  [[nodiscard]] Outcome execute(const ClientCommand& command);

  ReplicaConfig config_;
  OrderBook book_;
  std::map<std::uint64_t, Session> sessions_;
  std::uint64_t applied_index_ = 0;
};

}  // namespace rme::engine
