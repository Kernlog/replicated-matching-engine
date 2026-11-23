#include "sim/checker.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace rme::sim {
namespace {

using consensus::Entry;
using consensus::EntryType;
using consensus::Index;
using consensus::NodeId;
using consensus::RaftLog;
using consensus::Term;

Entry entry(std::uint64_t index, std::uint64_t term, std::uint8_t payload = 0) {
  return Entry{Term(term), Index(index), EntryType::kCommand, {static_cast<std::byte>(payload)}};
}

RaftLog log_of(std::initializer_list<std::uint64_t> terms, std::uint8_t payload = 0) {
  std::vector<Entry> entries;
  for (const std::uint64_t term : terms) {
    entries.push_back(entry(entries.size() + 1, term, payload));
  }
  return RaftLog(std::move(entries));
}

void expect_violation(const Checker& checker, const std::string& kind) {
  ASSERT_FALSE(checker.ok());
  EXPECT_NE(checker.violations().front().find(kind), std::string::npos)
      << checker.violations().front();
}

TEST(CheckerTest, CleanHistoryPasses) {
  Checker checker(3);
  const RaftLog log = log_of({1, 1, 2});
  checker.on_leader(NodeId(1), Term(1), log_of({}));
  checker.on_leader_commit(Term(1), Index(2), log);
  checker.on_leader(NodeId(2), Term(2), log);
  checker.on_leader_commit(Term(2), Index(3), log);
  for (const std::uint32_t node : {1U, 2U, 3U}) {
    for (std::uint64_t i = 1; i <= 3; ++i) {
      checker.on_applied(NodeId(node), log.at(Index(i)), 100 + i);
      checker.on_commit(NodeId(node), Index(i));
    }
  }
  checker.check_log_matching({&log, &log});
  EXPECT_TRUE(checker.ok());
  EXPECT_EQ(checker.committed(), 3U);
}

TEST(CheckerTest, TwoLeadersInOneTerm) {
  Checker checker(3);
  checker.on_leader(NodeId(1), Term(4), log_of({}));
  checker.on_leader(NodeId(2), Term(4), log_of({}));
  expect_violation(checker, "election safety");
}

TEST(CheckerTest, ApplyingADifferentEntry) {
  Checker checker(3);
  checker.on_leader_commit(Term(1), Index(1), log_of({1}, 7));
  checker.on_applied(NodeId(2), entry(1, 1, 8), 0);
  expect_violation(checker, "applied a different entry");
}

TEST(CheckerTest, ApplyingAnUncommittedEntry) {
  Checker checker(3);
  checker.on_applied(NodeId(2), entry(1, 1), 0);
  expect_violation(checker, "uncommitted");
}

TEST(CheckerTest, DivergentBookHash) {
  Checker checker(3);
  checker.on_leader_commit(Term(1), Index(1), log_of({1}));
  checker.on_applied(NodeId(1), entry(1, 1), 42);
  checker.on_applied(NodeId(2), entry(1, 1), 43);
  expect_violation(checker, "engine state");
}

TEST(CheckerTest, CommitMovingBackwardsWithoutRestart) {
  Checker checker(3);
  checker.on_commit(NodeId(1), Index(5));
  checker.on_restart(NodeId(1));
  checker.on_commit(NodeId(1), Index(0));
  EXPECT_TRUE(checker.ok());
  checker.on_commit(NodeId(1), Index(3));
  checker.on_commit(NodeId(1), Index(2));
  expect_violation(checker, "commit monotonicity");
}

TEST(CheckerTest, LeaderMissingAnEarlierCommittedEntry) {
  Checker checker(3);
  checker.on_leader_commit(Term(2), Index(2), log_of({1, 2}));
  checker.on_leader(NodeId(3), Term(3), log_of({1, 3}));
  expect_violation(checker, "leader completeness");
}

TEST(CheckerTest, LeaderOfAnOlderTermNeedNotHaveLaterCommits) {
  Checker checker(3);
  checker.on_leader_commit(Term(5), Index(1), log_of({5}));
  checker.on_leader(NodeId(1), Term(4), log_of({}));
  EXPECT_TRUE(checker.ok());
}

TEST(CheckerTest, LogsAgreeingOnATermButDifferingEarlier) {
  Checker checker(3);
  const RaftLog a = log_of({1, 2, 3}, 1);
  const RaftLog b = log_of({1, 2, 3}, 2);
  checker.check_log_matching({&a, &b});
  expect_violation(checker, "log matching");
}

TEST(CheckerTest, DivergentUncommittedSuffixesAreAllowed) {
  Checker checker(3);
  const RaftLog a = log_of({1, 1, 2, 2});
  const RaftLog b = log_of({1, 1, 3});
  checker.check_log_matching({&a, &b});
  EXPECT_TRUE(checker.ok());
}

engine::Outcome applied(std::uint64_t session, std::uint64_t seq) {
  engine::Outcome outcome;
  outcome.client = session;
  outcome.seq = seq;
  outcome.result = engine::SessionResult::kApplied;
  return outcome;
}

TEST(CheckerTest, CommandTakingEffectTwice) {
  Checker checker(3);
  const std::vector<engine::Outcome> once = {applied(7, 1)};
  checker.on_outcomes(Index(4), once);
  checker.on_outcomes(Index(4), once);
  EXPECT_TRUE(checker.ok());
  checker.on_outcomes(Index(9), once);
  expect_violation(checker, "exactly-once");
}

TEST(CheckerTest, DuplicatesAreNotSecondEffects) {
  Checker checker(3);
  checker.on_outcomes(Index(4), std::vector{applied(7, 1)});
  engine::Outcome duplicate = applied(7, 1);
  duplicate.result = engine::SessionResult::kDuplicate;
  checker.on_outcomes(Index(9), std::vector{duplicate});
  EXPECT_TRUE(checker.ok());
}

TEST(CheckerTest, AckedCommandHeldByAMajorityIsDurable) {
  Checker checker(3);
  const RaftLog full = log_of({1, 1});
  const RaftLog empty = log_of({});
  checker.on_leader_commit(Term(1), Index(2), full);
  checker.on_outcomes(Index(2), std::vector{applied(7, 1)});
  checker.check_acked({{7, 1}}, {&full, &full, &empty});
  EXPECT_TRUE(checker.ok());
  checker.check_acked({{7, 1}}, {&full, &empty, &empty});
  expect_violation(checker, "durability");
}

TEST(CheckerTest, AckedCommandThatNeverTookEffect) {
  Checker checker(3);
  const RaftLog full = log_of({1});
  checker.check_acked({{7, 1}}, {&full, &full, &full});
  expect_violation(checker, "never took effect");
}

}  // namespace
}  // namespace rme::sim
