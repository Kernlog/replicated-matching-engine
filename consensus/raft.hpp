#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "consensus/raft_log.hpp"
#include "consensus/types.hpp"
#include "core/rng.hpp"

namespace rme::consensus {

struct RaftConfig {
  NodeId id;
  std::vector<NodeId> members;
  std::uint32_t election_ticks = 10;
  std::uint32_t heartbeat_ticks = 1;
  std::uint64_t seed = 1;
  std::size_t max_entries_per_append = 64;
  std::size_t max_entry_bytes = std::size_t{64} << 10U;
  std::uint32_t max_inflight = 8;
  std::uint64_t max_uncommitted_entries = 256;
  bool pre_vote = true;
  bool check_quorum = true;
  // The node restarted without its durable state (disk lost or wiped), so it may have voted in
  // terms it no longer remembers and lost entries it helped commit.
  bool lost_state = false;
};

enum class Role : std::uint8_t { kFollower, kPreCandidate, kCandidate, kLeader };

class Raft {
 public:
  // A restart passes the durable snapshot (if any) and the entries that follow it; the state
  // machine must already be restored from that snapshot.
  explicit Raft(RaftConfig config, HardState state = {}, std::vector<Entry> log = {},
                std::optional<Snapshot> snapshot = std::nullopt);

  void tick();
  void step(const Message& message);
  // Buffers a command on the leader; commands are packed into one entry per Ready, up to
  // max_entry_bytes. Returns false when this node is not the leader, or when the leader already
  // holds max_uncommitted_entries it cannot commit yet, so overload pushes back on clients instead
  // of growing the log without bound.
  bool propose(std::span<const std::byte> command);

  [[nodiscard]] bool has_ready() const noexcept;
  Ready ready();
  // Storage reports entries up to index durable; the leader counts itself toward commit only then.
  void persisted(Index index);
  // Replaces the applied prefix of the log with the state machine image taken at applied().
  // The caller persists the returned snapshot.
  const Snapshot& compact(std::vector<std::byte> data);

  [[nodiscard]] NodeId id() const noexcept { return config_.id; }
  [[nodiscard]] Role role() const noexcept { return role_; }
  [[nodiscard]] Term term() const noexcept { return term_; }
  [[nodiscard]] std::optional<NodeId> leader() const noexcept { return leader_; }
  [[nodiscard]] const RaftLog& log() const noexcept { return log_; }
  [[nodiscard]] Index commit() const noexcept { return commit_; }
  [[nodiscard]] Index applied() const noexcept { return applied_; }
  [[nodiscard]] bool recovering() const noexcept { return recovering_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

 private:
  void become_follower(Term term, std::optional<NodeId> leader);
  void flush_proposals();
  void pre_campaign();
  void campaign();
  void become_leader();
  struct Progress {
    NodeId id;
    Index next;
    Index match;
    bool recent_active = false;
    bool recovering = false;
    bool probing = true;
    bool probe_sent = false;
    std::uint32_t stalled_ticks = 0;
  };

  void check_quorum();
  void broadcast_append();
  void broadcast_heartbeat();
  void replicate(Progress& peer);
  void heartbeat(Progress& peer);
  void probe_from(Progress& peer, Index next);
  std::size_t send_entries(const Progress& peer);
  Progress& progress_of(NodeId id);
  void maybe_commit();
  void reset_election_timer();
  void send(NodeId to, Body body);
  void send(NodeId to, Body body, Term term);
  [[nodiscard]] bool in_leader_lease() const noexcept;
  void note_term(NodeId peer, Term term);
  void probe_terms();
  void maybe_recover(Index leader_commit);
  [[nodiscard]] bool is_quorum(std::size_t count) const noexcept;
  [[nodiscard]] HardState hard_state() const noexcept { return {term_, vote_}; }

  void on(const Message& message, const VoteRequest& request);
  void on(const Message& message, const VoteResponse& response);
  void on(const Message& message, const AppendRequest& request);
  void on(const Message& message, const AppendResponse& response);
  void on(const Message& message, const InstallSnapshot& install);

  RaftConfig config_;
  Rng rng_;
  RaftLog log_;
  Role role_ = Role::kFollower;
  Term term_;
  std::optional<NodeId> vote_;
  std::optional<NodeId> leader_;
  HardState saved_;
  std::uint32_t elapsed_ = 0;
  std::uint32_t timeout_ = 0;
  std::uint32_t quorum_elapsed_ = 0;
  std::vector<NodeId> granted_;
  std::vector<Message> outbox_;
  std::vector<std::byte> pending_;
  std::vector<Progress> progress_;
  std::vector<Index> matches_;
  Index commit_;
  Index applied_;
  Index persisted_;
  std::uint64_t dropped_ = 0;
  std::optional<Snapshot> snapshot_;
  std::optional<Snapshot> installed_;
  bool recovering_ = false;
  std::uint32_t probe_elapsed_ = 0;
  std::vector<std::optional<Term>> peer_terms_;
};

}  // namespace rme::consensus
