#include "engine/replica.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <vector>

#include "codec/client_record.hpp"
#include "engine/book_hash.hpp"

namespace rme::engine {
namespace {

using Bytes = std::vector<std::byte>;

Bytes batch(std::initializer_list<codec::ClientRecordBytes> records) {
  Bytes out;
  out.reserve(records.size() * codec::kClientRecordSize);
  for (const codec::ClientRecordBytes& record : records) {
    out.insert(out.end(), record.begin(), record.end());
  }
  return out;
}

codec::ClientRecordBytes from(std::uint64_t client, std::uint64_t seq, const Command& command) {
  return codec::encode(ClientCommand{.client = client, .seq = seq, .command = command});
}

LimitOrder buy(std::uint64_t id, std::int64_t price, std::int64_t qty) {
  return {OrderId(id), Side::kBuy, Price(price), Qty(qty)};
}

LimitOrder sell(std::uint64_t id, std::int64_t price, std::int64_t qty) {
  return {OrderId(id), Side::kSell, Price(price), Qty(qty)};
}

std::vector<Outcome> apply(Replica& replica, std::uint64_t index, const Bytes& entry) {
  std::vector<Outcome> out;
  replica.apply(index, entry, out);
  return out;
}

std::uint64_t registered(Replica& replica, std::uint64_t index) {
  const std::vector<Outcome> out = apply(replica, index, batch({codec::encode_register()}));
  EXPECT_EQ(out.size(), 1U);
  return out.empty() ? 0 : out.front().client;
}

TEST(ReplicaTest, DuplicateReturnsCachedEventsWithoutEffect) {
  Replica replica;
  const std::uint64_t id = registered(replica, 1);
  apply(replica, 2, batch({from(id, 1, buy(1, 100, 10))}));
  const std::vector<Outcome> first = apply(replica, 3, batch({from(id, 2, sell(2, 100, 4))}));
  ASSERT_EQ(first.size(), 1U);
  EXPECT_EQ(first.front().result, SessionResult::kApplied);
  EXPECT_EQ(
      first.front().events,
      (std::vector<Event>{Accepted{OrderId(2)}, Fill{OrderId(2), OrderId(1), Price(100), Qty(4)}}));
  const std::uint64_t book = book_hash(replica.book());

  const std::vector<Outcome> retry =
      apply(replica, 4, batch({from(id, 2, sell(2, 100, 4)), from(id, 2, sell(9, 100, 6))}));
  const Outcome expected{
      .client = id, .seq = 2, .result = SessionResult::kDuplicate, .events = first.front().events};
  EXPECT_EQ(retry, (std::vector<Outcome>{expected, expected}));
  EXPECT_EQ(book_hash(replica.book()), book);
  EXPECT_EQ(replica.applied_index(), 4U);
}

TEST(ReplicaTest, StaleOutOfOrderAndUnknownSessionsHaveNoEffect) {
  Replica replica;
  const std::uint64_t id = registered(replica, 1);
  apply(replica, 2, batch({from(id, 1, buy(1, 100, 10)), from(id, 2, buy(2, 99, 10))}));
  const std::uint64_t book = book_hash(replica.book());
  const std::uint64_t never = std::uint64_t{7} << 20U;

  const std::vector<Outcome> rejected =
      apply(replica, 3,
            batch({from(id, 1, sell(3, 100, 10)), from(id, 4, sell(4, 99, 10)),
                   from(never, 1, sell(5, 99, 10)), from(id, 0, sell(6, 99, 10))}));
  EXPECT_EQ(rejected,
            (std::vector<Outcome>{
                {.client = id, .seq = 1, .result = SessionResult::kStale, .events = {}},
                {.client = id, .seq = 4, .result = SessionResult::kOutOfOrder, .events = {}},
                {.client = never, .seq = 1, .result = SessionResult::kUnknownSession, .events = {}},
                {.client = id, .seq = 0, .result = SessionResult::kStale, .events = {}},
            }));
  EXPECT_EQ(book_hash(replica.book()), book);

  const std::vector<Outcome> next = apply(replica, 4, batch({from(id, 3, Cancel{OrderId(1)})}));
  EXPECT_EQ(next, (std::vector<Outcome>{{.client = id,
                                         .seq = 3,
                                         .result = SessionResult::kApplied,
                                         .events = {Cancelled{OrderId(1), Qty(10)}}}}));
}

TEST(ReplicaTest, MalformedBatchHasNoEffect) {
  Replica replica;
  const std::uint64_t id = registered(replica, 1);
  Bytes entry = batch({from(id, 1, buy(1, 100, 10)), from(id, 2, buy(2, 100, 10))});
  entry.back() ^= std::byte{1};
  EXPECT_TRUE(apply(replica, 2, entry).empty());
  EXPECT_TRUE(apply(replica, 3, Bytes(5)).empty());
  EXPECT_EQ(replica.applied_index(), 3U);
  EXPECT_EQ(apply(replica, 4, batch({from(id, 1, buy(1, 100, 10))})).front().result,
            SessionResult::kApplied);
}

TEST(ReplicaTest, RegistrationIdsAreUniqueAndDeterministic) {
  const Bytes entry = batch({codec::encode_register(), codec::encode_register(),
                             from(1, 1, Cancel{}), codec::encode_register()});
  std::vector<std::uint64_t> ids;
  for (const std::uint64_t index : {1U, 2U, 9U}) {
    Replica a;
    Replica b;
    const std::vector<Outcome> left = apply(a, index, entry);
    EXPECT_EQ(apply(b, index, entry), left);
    EXPECT_EQ(book_hash(a.book()), book_hash(b.book()));
    for (const Outcome& outcome : left) {
      if (outcome.result == SessionResult::kRegistered) {
        EXPECT_NE(outcome.client, kRegisterClient);
        EXPECT_TRUE(outcome.events.empty());
        ids.push_back(outcome.client);
      }
    }
  }
  EXPECT_EQ(ids.size(), 9U);
  EXPECT_EQ(std::set(ids.begin(), ids.end()).size(), ids.size());

  Replica replica;
  const std::vector<Outcome> outcomes = apply(replica, 1, entry);
  for (std::size_t i = 0; i < 2; ++i) {
    const Outcome& outcome = outcomes.at(i);
    EXPECT_EQ(
        apply(replica, 2 + i, batch({from(outcome.client, 1, buy(10 + i, 100, 1))})).front().result,
        SessionResult::kApplied);
  }
}

}  // namespace
}  // namespace rme::engine
