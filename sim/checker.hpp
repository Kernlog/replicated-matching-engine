#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "consensus/raft_log.hpp"
#include "consensus/types.hpp"

namespace rme::sim {

// Raft safety invariants checked against observations from every node, as in spec/Raft.tla:
// election safety, log matching, leader completeness, state machine safety (same entry and same
// book hash at every applied index) and per-node commit monotonicity.
class Checker {
 public:
  explicit Checker(std::size_t nodes) : commit_floor_(nodes) {}

  void on_leader(consensus::NodeId node, consensus::Term term, const consensus::RaftLog& log);
  void on_leader_commit(consensus::Term term, consensus::Index commit,
                        const consensus::RaftLog& log);
  void on_commit(consensus::NodeId node, consensus::Index commit);
  void on_restart(consensus::NodeId node);
  void on_applied(consensus::NodeId node, const consensus::Entry& entry, std::uint64_t book_hash);
  void check_log_matching(const std::vector<const consensus::RaftLog*>& logs);

  [[nodiscard]] bool ok() const noexcept { return violations_.empty(); }
  [[nodiscard]] const std::vector<std::string>& violations() const noexcept { return violations_; }
  [[nodiscard]] std::size_t committed() const noexcept { return ledger_.size(); }

 private:
  struct Committed {
    consensus::Term term;
    std::uint64_t digest = 0;
    consensus::Term committed_in;
    std::uint64_t book_hash = 0;
    bool hashed = false;
  };

  void fail(std::string what);

  std::map<consensus::Term, consensus::NodeId> leaders_;
  std::vector<Committed> ledger_;
  std::vector<consensus::Index> commit_floor_;
  std::vector<std::string> violations_;
};

[[nodiscard]] std::uint64_t entry_digest(const consensus::Entry& entry);

}  // namespace rme::sim
