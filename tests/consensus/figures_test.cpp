#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

using Terms = std::vector<std::uint64_t>;

Terms terms_of(const std::vector<Entry>& entries) {
  Terms out;
  for (const Entry& entry : entries) {
    out.push_back(entry.term.value());
  }
  return out;
}

bool is_prefix(const std::vector<Entry>& a, const std::vector<Entry>& b) {
  const std::size_t n = std::min(a.size(), b.size());
  return std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(n), b.begin());
}

void expect_applied_consistent(Cluster& cluster, std::uint32_t size) {
  for (std::uint32_t a = 1; a <= size; ++a) {
    for (std::uint32_t b = a + 1; b <= size; ++b) {
      EXPECT_TRUE(is_prefix(cluster.applied(a), cluster.applied(b)))
          << "S" << a << " applied " << ::testing::PrintToString(terms_of(cluster.applied(a)))
          << ", S" << b << " applied " << ::testing::PrintToString(terms_of(cluster.applied(b)));
    }
  }
}

struct Figure7Follower {
  std::string name;
  Terms log;
};

// Raft paper Figure 7: the term-8 leader brings every follower to its log, whether the follower
// is missing entries (a, b, e), has extra uncommitted entries (c, d) or both (f).
TEST(FiguresTest, Figure7LeaderRepairsEveryFollower) {
  const Terms leader_log = {1, 1, 1, 4, 4, 5, 5, 6, 6, 6};
  const std::vector<Figure7Follower> followers = {
      {"a", {1, 1, 1, 4, 4, 5, 5, 6, 6}},
      {"b", {1, 1, 1, 4}},
      {"c", {1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 6}},
      {"d", {1, 1, 1, 4, 4, 5, 5, 6, 6, 6, 7, 7}},
      {"e", {1, 1, 1, 4, 4, 4, 4}},
      {"f", {1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3}},
  };
  Cluster cluster(7, kPaperRaft);
  cluster.load(1, 7, leader_log);
  for (std::uint32_t i = 0; i < followers.size(); ++i) {
    cluster.load(i + 2, 7, followers[i].log);
  }
  cluster.campaign(1);
  ASSERT_EQ(cluster.leader_id(), 1U);
  ASSERT_EQ(cluster.node(1).term(), Term(8));
  cluster.tick_all();

  Terms expected = leader_log;
  expected.push_back(8);
  for (std::uint32_t i = 0; i < followers.size(); ++i) {
    EXPECT_EQ(terms_of(cluster.log_of(i + 2)), expected) << followers[i].name;
    EXPECT_EQ(cluster.node(i + 2).commit(), Index(expected.size())) << followers[i].name;
  }
  expect_applied_consistent(cluster, 7);
}

// Raft paper Figure 8, state (c): S1 leads term 4 and has copied index 2 (term 2) to a majority.
void reach_figure8_c(Cluster& cluster) {
  cluster.load(1, 3, {1, 2});
  cluster.load(2, 2, {1, 2});
  cluster.load(3, 3, {1});
  cluster.load(4, 3, {1});
  cluster.load(5, 3, {1, 3});
  cluster.isolate(5);
  cluster.start_campaign(1);
  for (int round = 0; round < 6; ++round) {
    cluster.deliver_round();
  }
  ASSERT_EQ(cluster.leader_id(), 1U);
  ASSERT_EQ(cluster.node(1).term(), Term(4));
  for (const std::uint32_t id : {1U, 2U, 3U, 4U}) {
    ASSERT_GE(cluster.log_of(id).size(), 2U) << id;
    ASSERT_EQ(cluster.log_of(id)[1].term, Term(2)) << id;
  }
  ASSERT_EQ(cluster.log_of(3).size(), 2U);
}

TEST(FiguresTest, Figure8OldTermEntryOnMajorityIsNotCommitted) {
  Cluster cluster(5, {.max_entries = 1, .pre_vote = false, .check_quorum = false});
  reach_figure8_c(cluster);
  EXPECT_LT(cluster.node(1).commit(), Index(2));

  // State (d): S1 crashes and S5 wins term 5 with votes from S3 and S4, whose logs end in term 2.
  cluster.partition({{1}, {2, 3, 4, 5}});
  cluster.start_campaign(5);
  cluster.deliver();
  while (cluster.node(5).term() < Term(5)) {
    cluster.node(5).tick();
  }
  cluster.deliver();
  ASSERT_EQ(cluster.leader_id(), 5U);
  cluster.tick_all();
  EXPECT_EQ(cluster.log_of(3)[1].term, Term(3));
  cluster.heal();
  ASSERT_TRUE(cluster.run_until([&] { return cluster.logs_converged(); }));
  cluster.tick_all();
  expect_applied_consistent(cluster, 5);
}

TEST(FiguresTest, Figure8CurrentTermEntryOnMajorityCommitsBoth) {
  Cluster cluster(5, {.max_entries = 1, .pre_vote = false, .check_quorum = false});
  reach_figure8_c(cluster);
  cluster.deliver();
  cluster.tick_all();
  EXPECT_EQ(cluster.node(1).commit(), Index(3));

  cluster.partition({{1}, {2, 3, 4, 5}});
  for (int tick = 0; tick < 300; ++tick) {
    cluster.tick_all();
    ASSERT_NE(cluster.leader_id(), 5U) << tick;
  }
  cluster.heal();
  ASSERT_TRUE(cluster.run_until([&] { return cluster.logs_converged(); }));
  cluster.tick_all();
  for (std::uint32_t id = 1; id <= 5; ++id) {
    EXPECT_EQ(cluster.log_of(id)[1].term, Term(2)) << id;
  }
  expect_applied_consistent(cluster, 5);
}

}  // namespace
}  // namespace rme::consensus
