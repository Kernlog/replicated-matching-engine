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
      broadcast_append();
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

std::optional<Index> Raft::propose(std::vector<std::byte> data) {
  if (role_ != Role::kLeader) {
    return std::nullopt;
  }
  const Index index = log_.append(term_, EntryType::kCommand, std::move(data));
  progress_of(config_.id).match = index;
  broadcast_append();
  return index;
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
  progress_.clear();
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
  progress_.clear();
  for (const NodeId member : config_.members) {
    const Index match = member == config_.id ? log_.last_index() : Index(0);
    progress_.push_back(Progress{member, log_.last_index() + Index(1), match});
  }
  broadcast_append();
}

void Raft::broadcast_append() {
  for (const Progress& peer : progress_) {
    if (peer.id != config_.id) {
      send_append(peer);
    }
  }
}

void Raft::send_append(const Progress& peer) {
  const Index prev = peer.next - Index(1);
  const auto entries = log_.slice(peer.next, config_.max_entries_per_append);
  send(peer.id, AppendRequest{prev, log_.term_at(prev).value_or(Term(0)),
                              std::vector<Entry>(entries.begin(), entries.end()), Index(0)});
}

Raft::Progress& Raft::progress_of(NodeId id) {
  const auto found = std::ranges::find(progress_, id, &Progress::id);
  RME_ASSERT(found != progress_.end());
  return *found;
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

void Raft::on(const Message& message, const AppendRequest& request) {
  RME_ASSERT(role_ != Role::kLeader);
  become_follower(term_, message.from);
  if (!log_.matches(request.prev_index, request.prev_term)) {
    AppendResponse reject;
    if (request.prev_index > log_.last_index()) {
      reject.conflict_index = log_.last_index() + Index(1);
    } else {
      reject.conflict_term = log_.at(request.prev_index).term;
      reject.conflict_index = log_.first_index_of_term_at(request.prev_index);
    }
    send(message.from, reject);
    return;
  }
  const Index last_new = log_.merge(request.prev_index, request.entries);
  send(message.from, AppendResponse{true, last_new, Index(0), Term(0)});
}

void Raft::on(const Message& message, const AppendResponse& response) {
  if (role_ != Role::kLeader) {
    return;
  }
  Progress& peer = progress_of(message.from);
  if (response.success) {
    peer.match = std::max(peer.match, response.match);
    peer.next = std::max(peer.next, peer.match + Index(1));
    if (peer.next <= log_.last_index()) {
      send_append(peer);
    }
    return;
  }
  Index hinted = response.conflict_index;
  if (response.conflict_term != Term(0)) {
    if (const auto last = log_.last_index_of(response.conflict_term)) {
      hinted = *last + Index(1);
    }
  }
  peer.next = std::max(peer.match + Index(1), std::min(hinted, peer.next - Index(1)));
  send_append(peer);
}

}  // namespace rme::consensus
