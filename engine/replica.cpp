#include "engine/replica.hpp"

#include <algorithm>
#include <utility>

#include "codec/batch.hpp"
#include "core/assert.hpp"
#include "core/hash.hpp"
#include "engine/book_hash.hpp"
#include "engine/event_words.hpp"
#include "engine/snapshot.hpp"
#include "engine/wire.hpp"

namespace rme::engine {

namespace {

constexpr std::uint32_t kReplicaVersion = 2;
constexpr std::uint64_t kPositionBits = 20;
constexpr std::uint64_t kMaxBatchCommands = std::uint64_t{1} << kPositionBits;
constexpr std::uint64_t kMaxLogIndex = std::uint64_t{1} << (64 - kPositionBits);
constexpr std::size_t kEventSize = sizeof(EventWords);

SessionResult classify(std::uint64_t seq, std::uint64_t last_seq) noexcept {
  if (seq == last_seq) {
    return SessionResult::kDuplicate;
  }
  if (seq < last_seq) {
    return SessionResult::kStale;
  }
  return seq == last_seq + 1 ? SessionResult::kApplied : SessionResult::kOutOfOrder;
}

}  // namespace

Replica::Replica(ReplicaConfig config) : Replica(config, OrderBook(config.book)) {}

Replica::Replica(ReplicaConfig config, OrderBook book) : config_(config), book_(std::move(book)) {}

void Replica::apply(std::uint64_t log_index, std::span<const std::byte> batch,
                    std::vector<Outcome>& out) {
  RME_ASSERT(log_index > applied_index_ && log_index < kMaxLogIndex);
  applied_index_ = log_index;
  expire(log_index);
  const std::size_t count = batch.size() / codec::kClientRecordSize;
  sequenced_ += std::max<std::uint64_t>(count, 1);
  if (count > kMaxBatchCommands) {
    return;
  }
  out.reserve(out.size() + count);
  std::uint64_t position = 0;
  static_cast<void>(codec::for_each_client_command(batch, [&](const ClientCommand& command) {
    out.push_back(command.client == kRegisterClient ? open_session(log_index, position, command.seq)
                                                    : execute(log_index, command));
    ++position;
  }));
}

void Replica::expire(std::uint64_t log_index) {
  while (!by_last_index_.empty() &&
         log_index - by_last_index_.begin()->first > config_.session_ttl_entries) {
    sessions_.erase(by_last_index_.begin()->second);
    by_last_index_.erase(by_last_index_.begin());
  }
}

void Replica::touch(std::uint64_t id, Session& session, std::uint64_t log_index) {
  by_last_index_.erase({session.last_index, id});
  session.last_index = log_index;
  by_last_index_.emplace(log_index, id);
}

Outcome Replica::open_session(std::uint64_t log_index, std::uint64_t position,
                              std::uint64_t nonce) {
  const std::uint64_t id = (log_index << kPositionBits) | position;
  const bool inserted =
      sessions_.emplace(id, Session{.last_seq = 0, .last_index = log_index, .events = {}}).second;
  RME_ASSERT(inserted);
  by_last_index_.emplace(log_index, id);
  return {.client = id, .seq = nonce, .result = SessionResult::kRegistered, .events = {}};
}

Outcome Replica::execute(std::uint64_t log_index, const ClientCommand& command) {
  Outcome outcome{.client = command.client,
                  .seq = command.seq,
                  .result = SessionResult::kUnknownSession,
                  .events = {}};
  const auto found = sessions_.find(command.client);
  if (found == sessions_.end()) {
    return outcome;
  }
  Session& session = found->second;
  outcome.result = classify(command.seq, session.last_seq);
  if (outcome.result == SessionResult::kApplied) {
    session.last_seq = command.seq;
    touch(command.client, session, log_index);
    session.events.clear();
    book_.apply(command.command, session.events);
  }
  if (outcome.result == SessionResult::kApplied || outcome.result == SessionResult::kDuplicate) {
    outcome.events = session.events;
  }
  return outcome;
}

std::uint64_t Replica::hash() const {
  Hasher hasher;
  hasher.add(book_hash(book_));
  hasher.add(sessions_.size());
  for (const auto& [id, session] : sessions_) {
    hasher.add(id);
    hasher.add(session.last_seq);
    hasher.add(session.last_index);
    hasher.add(session.events.size());
    for (const Event& event : session.events) {
      for (const std::uint64_t w : to_words(event)) {
        hasher.add(w);
      }
    }
  }
  return hasher.digest();
}

std::vector<std::byte> Replica::snapshot() const {
  const std::vector<std::byte> book = engine::snapshot(book_);
  std::size_t size = 3 * sizeof(std::uint64_t) + book.size();
  for (const auto& entry : sessions_) {
    size += 4 * sizeof(std::uint64_t) + entry.second.events.size() * kEventSize;
  }
  wire::Writer out(kReplicaVersion, size);
  out.u64(applied_index_);
  out.u64(sequenced_);
  out.u64(sessions_.size());
  for (const auto& [id, session] : sessions_) {
    out.u64(id);
    out.u64(session.last_seq);
    out.u64(session.last_index);
    out.u64(session.events.size());
    for (const Event& event : session.events) {
      for (const std::uint64_t w : to_words(event)) {
        out.u64(w);
      }
    }
  }
  out.bytes(book);
  return std::move(out).seal();
}

std::optional<Replica> Replica::restore(std::span<const std::byte> bytes, ReplicaConfig config) {
  wire::Reader in(bytes, kReplicaVersion);
  const std::uint64_t applied_index = in.u64();
  const std::uint64_t sequenced = in.u64();
  const std::uint64_t session_count = in.u64();
  std::map<std::uint64_t, Session> sessions;
  for (std::uint64_t i = 0; i < session_count && in.ok(); ++i) {
    const std::uint64_t id = in.u64();
    const std::uint64_t last_seq = in.u64();
    const std::uint64_t last_index = in.u64();
    Session session{.last_seq = last_seq, .last_index = last_index, .events = {}};
    const std::uint64_t event_count = in.u64();
    const std::uint64_t registered_at = id >> kPositionBits;
    if (event_count > in.remaining() / kEventSize ||
        (!sessions.empty() && id <= sessions.rbegin()->first) || registered_at == 0 ||
        registered_at > last_index || last_index > applied_index ||
        applied_index - last_index > config.session_ttl_entries) {
      return std::nullopt;
    }
    session.events.reserve(event_count);
    for (std::uint64_t e = 0; e < event_count; ++e) {
      EventWords words{};
      for (std::uint64_t& w : words) {
        w = in.u64();
      }
      const std::optional<Event> event = from_words(words);
      if (!event) {
        return std::nullopt;
      }
      session.events.push_back(*event);
    }
    sessions.emplace_hint(sessions.end(), id, std::move(session));
  }
  if (!in.ok() || applied_index >= kMaxLogIndex) {
    return std::nullopt;
  }
  std::optional<OrderBook> book = engine::restore(in.take_rest(), config.book);
  if (!book) {
    return std::nullopt;
  }
  Replica replica(config, std::move(*book));
  replica.sessions_ = std::move(sessions);
  for (const auto& [id, session] : replica.sessions_) {
    replica.by_last_index_.emplace(session.last_index, id);
  }
  replica.applied_index_ = applied_index;
  replica.sequenced_ = sequenced;
  return replica;
}

}  // namespace rme::engine
