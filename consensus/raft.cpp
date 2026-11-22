#include "consensus/raft.hpp"

#include <algorithm>
#include <utility>
#include <variant>

#include "core/assert.hpp"

namespace rme::consensus {

Raft::Raft(RaftConfig config, HardState state, std::vector<Entry> log)
    : config_(std::move(config)),
      rng_(config_.seed),
      log_(std::move(log)),
      term_(state.term),
      vote_(state.vote),
      saved_(state) {
  RME_ASSERT(std::ranges::find(config_.members, config_.id) != config_.members.end());
  RME_ASSERT(config_.election_ticks > config_.heartbeat_ticks && config_.heartbeat_ticks > 0);
  reset_election_timer();
}

void Raft::tick() {
  ++elapsed_;
  if (role_ == Role::kLeader) {
    if (elapsed_ >= config_.heartbeat_ticks) {
      elapsed_ = 0;
      broadcast_heartbeat();
    }
    return;
  }
  if (elapsed_ >= timeout_) {
    campaign();
  }
}

void Raft::step(const Message& message) {
  RME_ASSERT(message.to == config_.id);
  if (message.term > term_) {
    const bool from_leader = std::holds_alternative<AppendRequest>(message.body);
    become_follower(message.term, from_leader ? std::optional(message.from) : std::nullopt);
  } else if (message.term < term_) {
    if (std::holds_alternative<AppendRequest>(message.body)) {
      send(message.from, AppendResponse{});
    }
    return;
  }
  std::visit([&](const auto& body) { on(message, body); }, message.body);
}

bool Raft::has_ready() const noexcept {
  return hard_state() != saved_ || !log_.unstable().empty() || !outbox_.empty();
}

Ready Raft::ready() {
  Ready ready;
  if (hard_state() != saved_) {
    saved_ = hard_state();
    ready.hard_state = saved_;
  }
  const auto unstable = log_.unstable();
  ready.entries.assign(unstable.begin(), unstable.end());
  log_.mark_stable(log_.last_index());
  ready.messages = std::exchange(outbox_, {});
  return ready;
}

void Raft::become_follower(Term term, std::optional<NodeId> leader) {
  if (term != term_) {
    term_ = term;
    vote_.reset();
  }
  role_ = Role::kFollower;
  leader_ = leader;
  granted_.clear();
  reset_election_timer();
}

void Raft::campaign() {
  role_ = Role::kCandidate;
  term_ += Term(1);
  vote_ = config_.id;
  leader_.reset();
  granted_ = {config_.id};
  reset_election_timer();
  if (is_quorum(granted_.size())) {
    become_leader();
    return;
  }
  for (const NodeId peer : config_.members) {
    if (peer != config_.id) {
      send(peer, VoteRequest{log_.last_index(), log_.last_term()});
    }
  }
}

void Raft::become_leader() {
  role_ = Role::kLeader;
  leader_ = config_.id;
  granted_.clear();
  elapsed_ = 0;
  broadcast_heartbeat();
}

void Raft::broadcast_heartbeat() {
  for (const NodeId peer : config_.members) {
    if (peer != config_.id) {
      send(peer, AppendRequest{log_.last_index(), log_.last_term(), {}, Index(0)});
    }
  }
}

void Raft::reset_election_timer() {
  elapsed_ = 0;
  timeout_ =
      config_.election_ticks + static_cast<std::uint32_t>(rng_.below(config_.election_ticks));
}

void Raft::send(NodeId to, Body body) {
  outbox_.push_back(Message{config_.id, to, term_, std::move(body)});
}

bool Raft::is_quorum(std::size_t count) const noexcept {
  return count * 2 > config_.members.size();
}

void Raft::on(const Message& message, const VoteRequest& request) {
  const bool up_to_date =
      request.last_term > log_.last_term() ||
      (request.last_term == log_.last_term() && request.last_index >= log_.last_index());
  const bool grant = up_to_date && (!vote_ || *vote_ == message.from);
  if (grant) {
    vote_ = message.from;
    reset_election_timer();
  }
  send(message.from, VoteResponse{grant});
}

void Raft::on(const Message& message, const VoteResponse& response) {
  if (role_ != Role::kCandidate || !response.granted ||
      std::ranges::find(granted_, message.from) != granted_.end()) {
    return;
  }
  granted_.push_back(message.from);
  if (is_quorum(granted_.size())) {
    become_leader();
  }
}

void Raft::on(const Message& message, const AppendRequest& /*request*/) {
  RME_ASSERT(role_ != Role::kLeader);
  become_follower(term_, message.from);
}

void Raft::on(const Message& /*message*/, const AppendResponse& /*response*/) {}

}  // namespace rme::consensus
