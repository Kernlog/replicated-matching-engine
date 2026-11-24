#include "node/node.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <variant>

#include "core/assert.hpp"

namespace rme::node {

namespace {

constexpr std::size_t kSpinsBeforeSleep = 20'000;
constexpr auto kIdleSleep = std::chrono::microseconds(50);

consensus::RaftConfig raft_config(const NodeConfig& config) {
  consensus::RaftConfig raft = config.raft;
  raft.id = config.id;
  raft.members.clear();
  for (const io::PeerAddress& member : config.members) {
    raft.members.push_back(member.id);
  }
  return raft;
}

const io::PeerAddress& self(const NodeConfig& config) {
  const auto found = std::ranges::find(config.members, config.id, &io::PeerAddress::id);
  if (found == config.members.end()) {
    throw std::invalid_argument("node id is not a member");
  }
  return *found;
}

engine::Replica restore(const std::optional<consensus::Snapshot>& snapshot,
                        const engine::ReplicaConfig& config) {
  if (!snapshot) {
    return engine::Replica(config);
  }
  std::optional<engine::Replica> replica = engine::Replica::restore(snapshot->data, config);
  if (!replica) {
    throw std::runtime_error("snapshot does not decode");
  }
  return std::move(*replica);
}

}  // namespace

Node::Node(NodeConfig config, OutcomeHandler on_outcome)
    : config_(std::move(config)),
      on_outcome_(std::move(on_outcome)),
      storage_(config_.data_dir),
      recovered_(storage_.take_recovered()),
      replica_(restore(recovered_.snapshot, config_.replica)),
      raft_(raft_config(config_), recovered_.state, std::move(recovered_.log), recovered_.snapshot),
      transport_(config_.id, config_.listen_host, self(config_).port,
                 [this](consensus::Message&& message) { inbox_.push(std::move(message)); }) {
  for (const io::PeerAddress& member : config_.members) {
    if (member.id != config_.id) {
      transport_.add_peer(member);
    }
  }
  outcomes_.reserve(64);
}

Node::~Node() {
  stop();
}

void Node::start() {
  storage_thread_.emplace(storage_);
  transport_.start();
  core_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void Node::stop() {
  if (core_.joinable()) {
    core_.request_stop();
    core_.join();
  }
  transport_.stop();
  storage_thread_.reset();
}

void Node::submit(std::vector<std::byte> command) noexcept {
  commands_.push(std::move(command));
}

NodeStatus Node::status() const noexcept {
  return NodeStatus{.role = published_.role.load(std::memory_order_relaxed),
                    .term = published_.term.load(std::memory_order_relaxed),
                    .leader = published_.leader.load(std::memory_order_relaxed),
                    .commit = published_.commit.load(std::memory_order_relaxed),
                    .applied = published_.applied.load(std::memory_order_relaxed),
                    .replica_hash = published_.replica_hash.load(std::memory_order_relaxed)};
}

void Node::run(const std::stop_token& stop) {
  using Clock = std::chrono::steady_clock;
  auto next_tick = Clock::now() + config_.tick;
  std::size_t idle = 0;
  while (!stop.stop_requested()) {
    bool busy = false;
    while (std::optional<consensus::Message> message = inbox_.try_pop()) {
      raft_.step(*message);
      busy = true;
    }
    busy = propose_commands() || busy;
    if (const auto now = Clock::now(); now >= next_tick) {
      raft_.tick();
      next_tick = std::max(next_tick + config_.tick, now);
      busy = true;
    }
    if (raft_.has_ready()) {
      handle_ready();
      busy = true;
    }
    busy = release() || busy;
    if (sent_) {
      transport_.flush();
      sent_ = false;
    }
    if (busy) {
      publish();
      idle = 0;
    } else if (++idle == 1) {
      published_.replica_hash.store(replica_.hash(), std::memory_order_relaxed);
    } else if (idle < kSpinsBeforeSleep) {
      cpu_relax();
    } else {
      std::this_thread::sleep_for(kIdleSleep);
    }
  }
}

// A leader at its uncommitted limit keeps the refused command and stops draining, so the ring
// fills and pushes back on the gateway; dropping it would leave a hole in the session's seqs and
// turn every later command of a pipelined client into out-of-order. A follower drops commands: the
// client resends them to the leader.
bool Node::propose_commands() {
  bool any = false;
  for (;;) {
    if (!deferred_) {
      deferred_ = commands_.try_pop();
      if (!deferred_) {
        return any;
      }
    }
    any = true;
    if (!raft_.propose(*deferred_) && raft_.role() == consensus::Role::kLeader) {
      return any;
    }
    deferred_.reset();
  }
}

// The leader's AppendEntries depend on nothing local, so they leave before the write is durable
// (Raft thesis 10.2.1). Every other message, and every apply, waits for this Ready's storage work
// and all earlier work.
void Node::handle_ready() {
  consensus::Ready ready = raft_.ready();
  Held held;
  for (consensus::Message& message : ready.messages) {
    if (std::holds_alternative<consensus::AppendRequest>(message.body)) {
      transport_.send(std::move(message));
      sent_ = true;
    } else {
      held.replies.push_back(std::move(message));
    }
  }
  if (ready.hard_state || ready.snapshot || !ready.entries.empty()) {
    io::StorageWork work;
    work.seq = ++seq_;
    work.state = ready.hard_state;
    work.snapshot = ready.snapshot;
    if (!ready.entries.empty()) {
      held.last = {ready.entries.back().index, ready.entries.back().term};
    }
    work.entries = std::move(ready.entries);
    storage_thread().submit(std::move(work));
  }
  held.seq = seq_;
  held.snapshot = std::move(ready.snapshot);
  held.committed = std::move(ready.committed);
  held_.push_back(std::move(held));
}

io::StorageThread& Node::storage_thread() noexcept {
  RME_ASSERT(storage_thread_.has_value());
  return *storage_thread_;  // NOLINT(bugprone-unchecked-optional-access)
}

bool Node::release() {
  const std::uint64_t synced = storage_thread().synced();
  bool released = false;
  while (!held_.empty() && held_.front().seq <= synced) {
    Held& held = held_.front();
    // Log Matching: the same index and term mean the same prefix, so a batch whose entries were
    // since replaced reports nothing.
    if (held.last && raft_.log().matches(held.last->first, held.last->second)) {
      raft_.persisted(held.last->first);
    }
    for (consensus::Message& message : held.replies) {
      transport_.send(std::move(message));
      sent_ = true;
    }
    if (held.snapshot) {
      replica_ = restore(held.snapshot, config_.replica);
    }
    for (const consensus::Entry& entry : held.committed) {
      apply(entry);
    }
    held_.pop_front();
    released = true;
  }
  return released;
}

void Node::apply(const consensus::Entry& entry) {
  outcomes_.clear();
  replica_.apply(entry.index.value(), entry.data, outcomes_);
  if (on_outcome_) {
    for (const engine::Outcome& outcome : outcomes_) {
      on_outcome_(entry.index.value(), outcome);
    }
  }
}

void Node::publish() {
  const auto leader = raft_.leader().value_or(consensus::NodeId(0)).value();
  if (raft_.role() != published_.role.load(std::memory_order_relaxed) ||
      raft_.term().value() != published_.term.load(std::memory_order_relaxed) ||
      leader != published_.leader.load(std::memory_order_relaxed)) {
    spdlog::info("node {} role {} term {} leader {} commit {}", config_.id.value(),
                 static_cast<int>(raft_.role()), raft_.term().value(), leader,
                 raft_.commit().value());
  }
  published_.role.store(raft_.role(), std::memory_order_relaxed);
  published_.leader.store(leader, std::memory_order_relaxed);
  published_.term.store(raft_.term().value(), std::memory_order_relaxed);
  published_.commit.store(raft_.commit().value(), std::memory_order_relaxed);
  published_.applied.store(replica_.applied_index(), std::memory_order_relaxed);
}

}  // namespace rme::node
