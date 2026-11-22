#include "consensus/raft_log.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace rme::consensus {
namespace {

Entry entry(std::uint64_t index, std::uint64_t term) {
  return Entry{Term(term), Index(index), EntryType::kCommand, {std::byte{0x01}}};
}

std::vector<std::uint64_t> terms(const RaftLog& log) {
  std::vector<std::uint64_t> out;
  for (Index i(1); i <= log.last_index(); i += Index(1)) {
    out.push_back(log.at(i).term.value());
  }
  return out;
}

RaftLog log_of(std::initializer_list<std::uint64_t> entry_terms) {
  std::vector<Entry> entries;
  std::uint64_t index = 0;
  for (const std::uint64_t term : entry_terms) {
    entries.push_back(entry(++index, term));
  }
  return RaftLog(std::move(entries));
}

TEST(RaftLogTest, EmptyLogHasSentinelAtZero) {
  const RaftLog log;
  EXPECT_EQ(log.last_index(), Index(0));
  EXPECT_EQ(log.last_term(), Term(0));
  EXPECT_EQ(log.term_at(Index(0)), Term(0));
  EXPECT_EQ(log.term_at(Index(1)), std::nullopt);
  EXPECT_TRUE(log.matches(Index(0), Term(0)));
}

TEST(RaftLogTest, AppendAssignsIndexesAndStaysUnstableUntilMarked) {
  RaftLog log;
  EXPECT_EQ(log.append(Term(1), EntryType::kNoop, {}), Index(1));
  EXPECT_EQ(log.append(Term(1), EntryType::kCommand, {std::byte{7}}), Index(2));
  EXPECT_EQ(log.unstable().size(), 2U);
  log.mark_stable(Index(1));
  ASSERT_EQ(log.unstable().size(), 1U);
  EXPECT_EQ(log.unstable().front().index, Index(2));
  EXPECT_EQ(log.last_term(), Term(1));
}

TEST(RaftLogTest, RestoredLogIsStable) {
  const RaftLog log = log_of({1, 1, 2});
  EXPECT_TRUE(log.unstable().empty());
  EXPECT_EQ(log.last_term(), Term(2));
}

TEST(RaftLogTest, MergeSkipsEntriesAlreadyPresent) {
  RaftLog log = log_of({1, 1, 2});
  const std::vector<Entry> request = {entry(2, 1), entry(3, 2)};
  EXPECT_EQ(log.merge(Index(1), request), Index(3));
  EXPECT_EQ(terms(log), (std::vector<std::uint64_t>{1, 1, 2}));
  EXPECT_TRUE(log.unstable().empty());
}

TEST(RaftLogTest, MergeKeepsSuffixBeyondAStaleShorterRequest) {
  RaftLog log = log_of({1, 1, 2});
  const std::vector<Entry> request = {entry(2, 1)};
  EXPECT_EQ(log.merge(Index(1), request), Index(2));
  EXPECT_EQ(terms(log), (std::vector<std::uint64_t>{1, 1, 2}));
}

TEST(RaftLogTest, MergeTruncatesAtFirstConflict) {
  RaftLog log = log_of({1, 1, 2, 2});
  const std::vector<Entry> request = {entry(2, 1), entry(3, 3)};
  EXPECT_EQ(log.merge(Index(1), request), Index(3));
  EXPECT_EQ(terms(log), (std::vector<std::uint64_t>{1, 1, 3}));
  EXPECT_EQ(log.stable_index(), Index(2));
  ASSERT_EQ(log.unstable().size(), 1U);
  EXPECT_EQ(log.unstable().front().term, Term(3));
}

TEST(RaftLogTest, MergeAppendsPastEnd) {
  RaftLog log = log_of({1});
  const std::vector<Entry> request = {entry(2, 1), entry(3, 1)};
  EXPECT_EQ(log.merge(Index(1), request), Index(3));
  EXPECT_EQ(terms(log), (std::vector<std::uint64_t>{1, 1, 1}));
}

TEST(RaftLogTest, SliceClampsToLogAndLimit) {
  const RaftLog log = log_of({1, 1, 2, 3});
  EXPECT_EQ(log.slice(Index(2), 2).size(), 2U);
  EXPECT_EQ(log.slice(Index(3), 10).size(), 2U);
  EXPECT_TRUE(log.slice(Index(5), 10).empty());
  EXPECT_TRUE(log.slice(Index(0), 10).empty());
}

TEST(RaftLogDeathTest, MergeRequiresContiguousEntries) {
  RaftLog log = log_of({1});
  const std::vector<Entry> gap = {entry(3, 1)};
  EXPECT_DEATH(log.merge(Index(1), gap), "assertion");
}

}  // namespace
}  // namespace rme::consensus
