#include "engine/replica.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "codec/client_record.hpp"
#include "core/rng.hpp"
#include "engine/book_hash.hpp"
#include "testing/command_stream.hpp"

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

constexpr ReplicaConfig ttl(std::uint64_t entries) {
  ReplicaConfig config;
  config.session_ttl_entries = entries;
  return config;
}

constexpr std::uint64_t kFakeClientBit = std::uint64_t{1} << 40U;

Replica round_trip(const Replica& replica, ReplicaConfig config = {}) {
  std::optional<Replica> copy = Replica::restore(replica.snapshot(), config);
  if (!copy) {
    ADD_FAILURE() << "valid snapshot rejected";
    return Replica{config};
  }
  return std::move(*copy);
}

std::size_t expired_in(const std::vector<Outcome>& outcomes) {
  std::size_t count = 0;
  for (const Outcome& outcome : outcomes) {
    if (outcome.result == SessionResult::kUnknownSession && outcome.client < kFakeClientBit) {
      ++count;
    }
  }
  return count;
}

// Builds a log of client batches with retries, stale, out-of-order and unknown sessions mixed
// in, replacing clients whose sessions expire.
class Workload {
 public:
  Workload(std::uint64_t seed, ReplicaConfig config)
      : rng_(seed), stream_(seed), scratch_(config) {}

  std::vector<Bytes> log(std::size_t entries) {
    std::vector<Bytes> out;
    out.reserve(entries);
    for (std::uint64_t index = 1; index <= entries; ++index) {
      out.push_back(entry());
      learn(apply(scratch_, index, out.back()));
    }
    return out;
  }

 private:
  struct Client {
    std::uint64_t id = 0;
    std::uint64_t next_seq = 1;
  };

  Bytes entry() {
    if (rng_.chance(1, 50)) {
      return Bytes(7);
    }
    Bytes out;
    const std::uint64_t count = clients_.empty() ? 2 : rng_.below(6);
    out.reserve(count * codec::kClientRecordSize);
    for (std::uint64_t i = 0; i < count; ++i) {
      const codec::ClientRecordBytes record = next_record();
      out.insert(out.end(), record.begin(), record.end());
    }
    return out;
  }

  codec::ClientRecordBytes next_record() {
    if (clients_.empty() || rng_.chance(1, 50)) {
      return codec::encode_register();
    }
    Client& client = clients_[rng_.below(clients_.size())];
    const std::uint64_t roll = rng_.below(100);
    if (roll < 10) {
      return from(client.id, client.next_seq - 1, Cancel{});
    }
    if (roll < 14) {
      return from(client.id, rng_.below(client.next_seq), Cancel{});
    }
    if (roll < 18) {
      return from(client.id, client.next_seq + 1 + rng_.below(3), Cancel{});
    }
    if (roll < 21) {
      return from(client.id | kFakeClientBit, 1, Cancel{});
    }
    return from(client.id, client.next_seq++, stream_.next());
  }

  void learn(const std::vector<Outcome>& outcomes) {
    for (const Outcome& outcome : outcomes) {
      if (outcome.result == SessionResult::kRegistered) {
        clients_.push_back({.id = outcome.client, .next_seq = 1});
      }
      if (outcome.result == SessionResult::kUnknownSession) {
        std::erase_if(clients_, [&](const Client& c) { return c.id == outcome.client; });
      }
    }
  }

  Rng rng_;
  testing::CommandStream stream_;
  Replica scratch_;
  std::vector<Client> clients_;
};

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
  const std::uint64_t hash = replica.hash();
  const std::uint64_t book = book_hash(replica.book());

  const std::vector<Outcome> retry =
      apply(replica, 4, batch({from(id, 2, sell(2, 100, 4)), from(id, 2, sell(9, 100, 6))}));
  const Outcome expected{
      .client = id, .seq = 2, .result = SessionResult::kDuplicate, .events = first.front().events};
  EXPECT_EQ(retry, (std::vector<Outcome>{expected, expected}));
  EXPECT_EQ(replica.hash(), hash);
  EXPECT_EQ(book_hash(replica.book()), book);
  EXPECT_EQ(replica.applied_index(), 4U);
}

TEST(ReplicaTest, StaleOutOfOrderAndUnknownSessionsHaveNoEffect) {
  Replica replica;
  const std::uint64_t id = registered(replica, 1);
  apply(replica, 2, batch({from(id, 1, buy(1, 100, 10)), from(id, 2, buy(2, 99, 10))}));
  const std::uint64_t hash = replica.hash();
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
  EXPECT_EQ(replica.hash(), hash);
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
  const std::uint64_t hash = replica.hash();
  Bytes entry = batch({from(id, 1, buy(1, 100, 10)), from(id, 2, buy(2, 100, 10))});
  entry.back() ^= std::byte{1};
  EXPECT_TRUE(apply(replica, 2, entry).empty());
  EXPECT_TRUE(apply(replica, 3, Bytes(5)).empty());
  EXPECT_EQ(replica.hash(), hash);
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
    EXPECT_EQ(a.hash(), b.hash());
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

TEST(ReplicaTest, SnapshotRoundTripsHashAndLaterAppliesStayIdentical) {
  for (const ReplicaConfig config : {ReplicaConfig{}, ttl(6)}) {
    const std::vector<Bytes> log = Workload(21, config).log(800);
    for (const std::size_t prefix : std::array<std::size_t, 4>{1, 2, 200, 600}) {
      Replica original(config);
      for (std::size_t i = 0; i < prefix; ++i) {
        apply(original, i + 1, log[i]);
      }
      Replica restored = round_trip(original, config);
      ASSERT_EQ(restored.hash(), original.hash()) << prefix;
      EXPECT_EQ(restored.applied_index(), original.applied_index());
      EXPECT_EQ(restored.snapshot(), original.snapshot());
      std::size_t applied = 0;
      for (std::size_t i = prefix; i < log.size(); ++i) {
        const std::vector<Outcome> expected = apply(original, i + 1, log[i]);
        ASSERT_EQ(apply(restored, i + 1, log[i]), expected) << prefix << " entry " << i;
        for (const Outcome& outcome : expected) {
          applied += outcome.result == SessionResult::kApplied ? 1 : 0;
        }
      }
      EXPECT_GT(applied, log.size() - prefix);
      EXPECT_EQ(restored.hash(), original.hash()) << prefix;
      EXPECT_EQ(book_hash(restored.book()), book_hash(original.book()));
    }
  }
}

TEST(ReplicaTest, IdenticalLogsExpireIdentically) {
  const ReplicaConfig config = ttl(4);
  const std::vector<Bytes> log = Workload(33, config).log(1'500);
  Replica a(config);
  Replica b(config);
  Replica never_expires;
  std::size_t expired = 0;
  for (std::size_t i = 0; i < log.size(); ++i) {
    const std::vector<Outcome> left = apply(a, i + 1, log[i]);
    ASSERT_EQ(apply(b, i + 1, log[i]), left) << i;
    ASSERT_EQ(a.hash(), b.hash()) << i;
    expired += expired_in(left);
    apply(never_expires, i + 1, log[i]);
  }
  EXPECT_GT(expired, 20U);
  EXPECT_EQ(expired_in(apply(never_expires, log.size() + 1, {})), 0U);
  EXPECT_NE(never_expires.hash(), a.hash());
}

TEST(ReplicaTest, RejectsCorruptAndTruncatedSnapshots) {
  const std::vector<Bytes> log = Workload(4, {}).log(40);
  Replica replica;
  for (std::size_t i = 0; i < log.size(); ++i) {
    apply(replica, i + 1, log[i]);
  }
  const Bytes clean = replica.snapshot();
  for (std::size_t i = 0; i < clean.size(); ++i) {
    Bytes corrupt = clean;
    corrupt[i] ^= std::byte{0x10};
    EXPECT_FALSE(Replica::restore(corrupt).has_value()) << "byte " << i;
    EXPECT_FALSE(Replica::restore(std::span(clean).first(i)).has_value()) << "length " << i;
  }
}

}  // namespace
}  // namespace rme::engine
