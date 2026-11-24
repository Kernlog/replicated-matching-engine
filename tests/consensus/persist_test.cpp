#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "consensus/node.hpp"
#include "consensus/raft.hpp"
#include "tests/consensus/cluster.hpp"

namespace rme::consensus {
namespace {

std::string label(const Message& message) {
  if (std::holds_alternative<VoteResponse>(message.body)) {
    return "send vote";
  }
  if (std::holds_alternative<AppendRequest>(message.body)) {
    return "send append";
  }
  return "send";
}

struct Recorder {
  std::vector<std::string> events;
  std::vector<Message> sent;

  void save(const HardState& /*state*/) { events.emplace_back("save"); }
  void append(std::span<const Entry> /*entries*/) { events.emplace_back("append"); }
  void save_snapshot(const Snapshot& /*snapshot*/) { events.emplace_back("snapshot"); }
  void sync() { events.emplace_back("sync"); }
  void send(Message&& message) {
    events.emplace_back(label(message));
    sent.push_back(std::move(message));
  }
  void apply(const Entry& /*entry*/) { events.emplace_back("apply"); }
  void restore(const Snapshot& /*snapshot*/) { events.emplace_back("restore"); }
};

TEST(PersistTest, VoteIsSyncedBeforeTheGrantIsSent) {
  Raft raft(RaftConfig{.id = NodeId(1), .members = {NodeId(1), NodeId(2), NodeId(3)}});
  raft.step({NodeId(2), NodeId(1), Term(1), VoteRequest{}});
  Recorder recorder;
  process_ready(raft, recorder, recorder, recorder);
  EXPECT_EQ(recorder.events, (std::vector<std::string>{"save", "sync", "send vote"}));
}

TEST(PersistTest, NothingIsSyncedWithoutDurableChanges) {
  Raft raft(RaftConfig{.id = NodeId(1), .members = {NodeId(1), NodeId(2), NodeId(3)}},
            HardState{Term(2), std::nullopt});
  raft.step({NodeId(2), NodeId(1), Term(1), AppendRequest{}});
  Recorder recorder;
  process_ready(raft, recorder, recorder, recorder);
  EXPECT_EQ(recorder.events, (std::vector<std::string>{"send"}));
}

TEST(PersistTest, VoteSurvivesRestart) {
  Cluster cluster(3);
  cluster.isolate(3);
  cluster.campaign(1);
  ASSERT_EQ(cluster.leader_id(), 1U);
  const Term term = cluster.node(1).term();
  EXPECT_EQ(cluster.storage(2).state(), (HardState{term, NodeId(1)}));

  cluster.restart(2);
  EXPECT_EQ(cluster.node(2).term(), term);
  cluster.node(2).step({NodeId(3), NodeId(2), term, VoteRequest{}});
  const Ready ready = cluster.node(2).ready();
  ASSERT_EQ(ready.messages.size(), 1U);
  EXPECT_FALSE(std::get<VoteResponse>(ready.messages[0].body).granted);
}

TEST(PersistTest, RestartedClusterElectsInAHigherTerm) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  const Term term = cluster.node(cluster.leader_id()).term();
  for (std::uint32_t id = 1; id <= 3; ++id) {
    cluster.restart(id);
  }
  ASSERT_TRUE(cluster.run_until([&] { return cluster.leader_id() != 0; }));
  EXPECT_GT(cluster.node(cluster.leader_id()).term(), term);
}

Raft leader_of_three() {
  Raft raft(
      RaftConfig{.id = NodeId(1), .members = {NodeId(1), NodeId(2), NodeId(3)}, .pre_vote = false});
  while (raft.role() != Role::kCandidate) {
    raft.tick();
  }
  raft.step({NodeId(2), NodeId(1), raft.term(), VoteResponse{true, false}});
  EXPECT_EQ(raft.role(), Role::kLeader);
  Recorder ignore;
  process_ready(raft, ignore, ignore, ignore);
  const Index noop = raft.log().last_index();
  for (const std::uint32_t follower : {2U, 3U}) {
    raft.step({NodeId(follower), NodeId(1), raft.term(),
               AppendResponse{true, noop, Index(0), Term(0), false}});
  }
  EXPECT_EQ(raft.commit(), noop);
  process_ready(raft, ignore, ignore, ignore);
  return raft;
}

TEST(SelfAckTest, LeaderSendsAppendsBeforeItsOwnSync) {
  Raft raft = leader_of_three();
  const std::array command{std::byte{1}};
  ASSERT_TRUE(raft.propose(command));
  Recorder recorder;
  process_ready(raft, recorder, recorder, recorder);
  EXPECT_EQ(recorder.events,
            (std::vector<std::string>{"send append", "send append", "append", "sync"}));
}

TEST(SelfAckTest, LeaderCountsItselfOnlyAfterPersist) {
  Raft raft = leader_of_three();
  const Index committed = raft.commit();
  const std::array command{std::byte{1}};
  ASSERT_TRUE(raft.propose(command));
  const Ready ready = raft.ready();
  const Index index = ready.entries.back().index;
  raft.step(
      {NodeId(2), NodeId(1), raft.term(), AppendResponse{true, index, Index(0), Term(0), false}});
  EXPECT_EQ(raft.commit(), committed);
  raft.persisted(index);
  EXPECT_EQ(raft.commit(), index);
}

TEST(SelfAckTest, FollowersAloneFormAMajorityBeforeLeaderFsync) {
  Raft raft = leader_of_three();
  const std::array command{std::byte{1}};
  ASSERT_TRUE(raft.propose(command));
  const Index index = raft.ready().entries.back().index;
  for (const std::uint32_t follower : {2U, 3U}) {
    raft.step({NodeId(follower), NodeId(1), raft.term(),
               AppendResponse{true, index, Index(0), Term(0), false}});
  }
  EXPECT_EQ(raft.commit(), index);
}

}  // namespace
}  // namespace rme::consensus
