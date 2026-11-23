#include "engine/replica.hpp"

#include "codec/batch.hpp"
#include "core/assert.hpp"

namespace rme::engine {

namespace {

constexpr std::uint64_t kPositionBits = 20;
constexpr std::uint64_t kMaxBatchCommands = std::uint64_t{1} << kPositionBits;
constexpr std::uint64_t kMaxLogIndex = std::uint64_t{1} << (64 - kPositionBits);

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

Replica::Replica(ReplicaConfig config) : config_(config), book_(config.book) {}

void Replica::apply(std::uint64_t log_index, std::span<const std::byte> batch,
                    std::vector<Outcome>& out) {
  RME_ASSERT(log_index > applied_index_ && log_index < kMaxLogIndex);
  applied_index_ = log_index;
  const std::size_t count = batch.size() / codec::kClientRecordSize;
  if (count > kMaxBatchCommands) {
    return;
  }
  out.reserve(out.size() + count);
  std::uint64_t position = 0;
  static_cast<void>(codec::for_each_client_command(batch, [&](const ClientCommand& command) {
    out.push_back(command.client == kRegisterClient ? open_session(log_index, position)
                                                    : execute(command));
    ++position;
  }));
}

Outcome Replica::open_session(std::uint64_t log_index, std::uint64_t position) {
  const std::uint64_t id = (log_index << kPositionBits) | position;
  const bool inserted = sessions_.emplace(id, Session{}).second;
  RME_ASSERT(inserted);
  return {.client = id, .result = SessionResult::kRegistered, .events = {}};
}

Outcome Replica::execute(const ClientCommand& command) {
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
    session.events.clear();
    book_.apply(command.command, session.events);
  }
  if (outcome.result == SessionResult::kApplied || outcome.result == SessionResult::kDuplicate) {
    outcome.events = session.events;
  }
  return outcome;
}

}  // namespace rme::engine
