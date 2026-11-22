#include "consensus/types.hpp"

#include <gtest/gtest.h>

#include <span>

#include "consensus/concepts.hpp"

namespace rme::consensus {
namespace {

struct FullStorage {
  void save(const HardState& /*state*/) {}
  void append(std::span<const Entry> /*entries*/) {}
  void sync() {}
};

struct NoSyncStorage {
  void save(const HardState& /*state*/) {}
  void append(std::span<const Entry> /*entries*/) {}
};

struct Wire {
  void send(const Message& /*message*/) {}
};

struct Machine {
  void apply(const Entry& /*entry*/) {}
};

static_assert(Storage<FullStorage>);
static_assert(!Storage<NoSyncStorage>);
static_assert(Transport<Wire>);
static_assert(!Transport<FullStorage>);
static_assert(StateMachine<Machine>);

TEST(ReadyTest, EmptyOnlyWithoutAnyEffect) {
  EXPECT_TRUE(Ready{}.empty());
  Ready hard_state;
  hard_state.hard_state = HardState{};
  EXPECT_FALSE(hard_state.empty());
  Ready entries;
  entries.entries.emplace_back();
  EXPECT_FALSE(entries.empty());
  Ready messages;
  messages.messages.emplace_back();
  EXPECT_FALSE(messages.empty());
  Ready committed;
  committed.committed.emplace_back();
  EXPECT_FALSE(committed.empty());
}

TEST(MessageTest, ComparesBodies) {
  const Message vote{NodeId(1), NodeId(2), Term(3), VoteRequest{Index(4), Term(2), false}};
  Message other = vote;
  EXPECT_EQ(vote, other);
  other.body = VoteResponse{true, false};
  EXPECT_NE(vote, other);
}

}  // namespace
}  // namespace rme::consensus
