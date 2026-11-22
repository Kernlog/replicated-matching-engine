#include "consensus/raft.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
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
    if (config_.pre_vote) {
      pre_campaign();
    } else {
      campaign();
    }
  }
}

void Raft::step(const Message& message) {
  if (message.to != config_.id || message.from == config_.id ||
      std::ranges::find(config_.members, message.from) == config_.members.end()) {
    ++dropped_;
    return;
  }
  const auto* vote_request = std::get_if<VoteRequest>(&message.body);
  const auto* vote_response = std::get_if<VoteResponse>(&message.body);
  const bool pre_vote_request = vote_request != nullptr && vote_request->pre_vote;
  const bool pre_vote_grant =
      vote_response != nullptr && vote_response->pre_vote && vote_response->granted;
  if (message.term > term_) {
    if (!pre_vote_request && !pre_vote_grant) {
      const bool from_leader = std::holds_alternative<AppendRequest>(message.body);
      become_follower(message.term, from_leader ? std::optional(message.from) : std::nullopt);
    }
  } else if (message.term < term_) {
    if (std::holds_alternative<AppendRequest>(message.body)) {
      send(message.from, AppendResponse{});
    } else if (pre_vote_request) {
      send(message.from, VoteResponse{false, true});
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
  maybe_commit();
  broadcast_append();
  return index;
}

bool Raft::has_ready() const noexcept {
  return hard_state() != saved_ || !log_.unstable().empty() || !outbox_.empty() ||
         commit_ > applied_;
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
  const auto committed = log_.slice(applied_ + Index(1), (commit_ - applied_).value());
  ready.committed.assign(committed.begin(), committed.end());
  applied_ = commit_;
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

// A pre-election at term + 1 that changes no state, so a node that cannot reach a quorum (or
// rejoins while the leader is healthy) never bumps the term and deposes a working leader.
void Raft::pre_campaign() {
  role_ = Role::kPreCandidate;
  leader_.reset();
  granted_ = {config_.id};
  reset_election_timer();
  if (is_quorum(granted_.size())) {
    campaign();
    return;
  }
  for (const NodeId peer : config_.members) {
    if (peer != config_.id) {
      send(peer, VoteRequest{log_.last_index(), log_.last_term(), true}, term_ + Term(1));
    }
  }
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
      send(peer, VoteRequest{log_.last_index(), log_.last_term(), false});
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
  progress_of(config_.id).match = log_.append(term_, EntryType::kNoop, {});
  maybe_commit();
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
                              std::vector<Entry>(entries.begin(), entries.end()), commit_});
}

// Only an entry from the current term is committed by counting replicas (paper section 5.4.2);
// earlier entries commit with it.
void Raft::maybe_commit() {
  matches_.clear();
  for (const Progress& peer : progress_) {
    matches_.push_back(peer.match);
  }
  const auto median = matches_.begin() + static_cast<std::ptrdiff_t>(matches_.size() / 2);
  std::ranges::nth_element(matches_, median, std::greater{});
  if (*median > commit_ && log_.term_at(*median) == term_) {
    commit_ = *median;
  }
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
  send(to, std::move(body), term_);
}

void Raft::send(NodeId to, Body body, Term term) {
  outbox_.push_back(Message{config_.id, to, term, std::move(body)});
}

bool Raft::in_leader_lease() const noexcept {
  return leader_.has_value() && elapsed_ < config_.election_ticks;
}

bool Raft::is_quorum(std::size_t count) const noexcept {
  return count * 2 > config_.members.size();
}

void Raft::on(const Message& message, const VoteRequest& request) {
  const bool up_to_date =
      request.last_term > log_.last_term() ||
      (request.last_term == log_.last_term() && request.last_index >= log_.last_index());
  if (request.pre_vote) {
    const bool grant = up_to_date && message.term > term_ && !in_leader_lease();
    send(message.from, VoteResponse{grant, true}, grant ? message.term : term_);
    return;
  }
  const bool grant = up_to_date && (!vote_ || *vote_ == message.from);
  if (grant) {
    vote_ = message.from;
    reset_election_timer();
  }
  send(message.from, VoteResponse{grant, false});
}

void Raft::on(const Message& message, const VoteResponse& response) {
  const Role expected = response.pre_vote ? Role::kPreCandidate : Role::kCandidate;
  if (role_ != expected || !response.granted ||
      std::ranges::find(granted_, message.from) != granted_.end()) {
    return;
  }
  granted_.push_back(message.from);
  if (!is_quorum(granted_.size())) {
    return;
  }
  if (response.pre_vote) {
    campaign();
  } else {
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
  commit_ = std::max(commit_, std::min(request.commit, last_new));
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
    maybe_commit();
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
