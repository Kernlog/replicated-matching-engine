#include "sim/checker.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "core/hash.hpp"

namespace rme::sim {

namespace {

constexpr std::size_t kMaxViolations = 16;

std::string at(consensus::Index index) {
  return " at index " + std::to_string(index.value());
}

std::string node_name(consensus::NodeId node) {
  return "node " + std::to_string(node.value());
}

}  // namespace

std::uint64_t entry_digest(const consensus::Entry& entry) {
  Hasher hasher;
  hasher.add(entry.term.value());
  hasher.add(entry.index.value());
  hasher.add(static_cast<std::uint64_t>(entry.type));
  hasher.add(entry.data.size());
  for (const std::byte byte : entry.data) {
    hasher.add(static_cast<std::uint64_t>(byte));
  }
  return hasher.digest();
}

void Checker::on_leader(consensus::NodeId node, consensus::Term term,
                        const consensus::RaftLog& log) {
  const auto [it, inserted] = leaders_.try_emplace(term, node);
  if (!inserted && it->second != node) {
    fail("election safety: " + node_name(it->second) + " and " + node_name(node) +
         " both lead term " + std::to_string(term.value()));
  }
  for (std::size_t i = 0; i < ledger_.size(); ++i) {
    const Committed& entry = ledger_[i];
    const consensus::Index index(i + 1);
    if (entry.committed_in < term && !log.matches(index, entry.term)) {
      fail("leader completeness: " + node_name(node) + " leads term " +
           std::to_string(term.value()) + " without the entry committed in term " +
           std::to_string(entry.committed_in.value()) + at(index));
      return;
    }
  }
}

void Checker::on_leader_commit(consensus::Term term, consensus::Index commit,
                               const consensus::RaftLog& log) {
  if (commit.value() <= ledger_.size()) {
    if (commit > consensus::Index(0) &&
        ledger_[commit.value() - 1].digest != entry_digest(log.at(commit))) {
      fail("state machine safety: a term " + std::to_string(term.value()) +
           " leader committed a different entry" + at(commit));
    }
    return;
  }
  for (consensus::Index index(ledger_.size() + 1); index <= commit; index += consensus::Index(1)) {
    const consensus::Entry& entry = log.at(index);
    ledger_.push_back(Committed{entry.term, entry_digest(entry), term, 0, false});
  }
}

void Checker::on_commit(consensus::NodeId node, consensus::Index commit) {
  consensus::Index& floor = commit_floor_.at(node.value() - 1);
  if (commit < floor) {
    fail("commit monotonicity: " + node_name(node) + " moved commit from " +
         std::to_string(floor.value()) + " to " + std::to_string(commit.value()));
  }
  floor = std::max(floor, commit);
}

void Checker::on_restart(consensus::NodeId node) {
  commit_floor_.at(node.value() - 1) = consensus::Index(0);
}

void Checker::on_applied(consensus::NodeId node, const consensus::Entry& entry,
                         std::uint64_t book_hash) {
  if (entry.index.value() > ledger_.size()) {
    fail("state machine safety: " + node_name(node) + " applied an uncommitted entry" +
         at(entry.index));
    return;
  }
  Committed& committed = ledger_[entry.index.value() - 1];
  if (committed.digest != entry_digest(entry)) {
    fail("state machine safety: " + node_name(node) + " applied a different entry" +
         at(entry.index));
    return;
  }
  if (!committed.hashed) {
    committed.book_hash = book_hash;
    committed.hashed = true;
  } else if (committed.book_hash != book_hash) {
    fail("state machine safety: " + node_name(node) + " has a different book_hash" +
         at(entry.index));
  }
}

void Checker::check_log_matching(const std::vector<const consensus::RaftLog*>& logs) {
  for (std::size_t a = 0; a < logs.size(); ++a) {
    for (std::size_t b = a + 1; b < logs.size(); ++b) {
      const consensus::RaftLog& left = *logs[a];
      const consensus::RaftLog& right = *logs[b];
      const consensus::Index shared = std::min(left.last_index(), right.last_index());
      bool matched = false;
      for (consensus::Index index = shared; index > consensus::Index(0);
           index -= consensus::Index(1)) {
        const bool same_term = left.at(index).term == right.at(index).term;
        matched = matched || same_term;
        if (matched && entry_digest(left.at(index)) != entry_digest(right.at(index))) {
          fail("log matching: logs " + std::to_string(a + 1) + " and " + std::to_string(b + 1) +
               " agree on a later term but differ" + at(index));
          return;
        }
      }
    }
  }
}

void Checker::fail(std::string what) {
  if (violations_.size() < kMaxViolations) {
    violations_.push_back(std::move(what));
  }
}

}  // namespace rme::sim
