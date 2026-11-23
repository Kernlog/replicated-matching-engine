#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "codec/client_record.hpp"
#include "engine/book_hash.hpp"
#include "engine/replica.hpp"

namespace rme::engine {
namespace {

using Bytes = std::vector<std::byte>;

constexpr ReplicaConfig ttl(std::uint64_t entries) {
  ReplicaConfig config;
  config.session_ttl_entries = entries;
  return config;
}

constexpr ReplicaConfig kTtl10 = ttl(10);

Bytes batch(std::span<const codec::ClientRecordBytes> records) {
  Bytes out;
  out.reserve(records.size() * codec::kClientRecordSize);
  for (const codec::ClientRecordBytes& record : records) {
    out.insert(out.end(), record.begin(), record.end());
  }
  return out;
}

Bytes one(std::uint64_t client, std::uint64_t seq, const Command& command) {
  const std::array<codec::ClientRecordBytes, 1> record{
      codec::encode(ClientCommand{.client = client, .seq = seq, .command = command})};
  return batch(record);
}

LimitOrder buy(std::uint64_t id) {
  return {OrderId(id), Side::kBuy, Price(100), Qty(10)};
}

std::vector<Outcome> apply(Replica& replica, std::uint64_t index, const Bytes& entry) {
  std::vector<Outcome> out;
  replica.apply(index, entry, out);
  return out;
}

SessionResult result(Replica& replica, std::uint64_t index, const Bytes& entry) {
  const std::vector<Outcome> out = apply(replica, index, entry);
  EXPECT_EQ(out.size(), 1U);
  return out.empty() ? SessionResult::kRegistered : out.front().result;
}

std::uint64_t registered(Replica& replica, std::uint64_t index) {
  const std::array<codec::ClientRecordBytes, 1> record{codec::encode_register()};
  const std::vector<Outcome> out = apply(replica, index, batch(record));
  EXPECT_EQ(out.size(), 1U);
  return out.empty() ? 0 : out.front().client;
}

TEST(SessionExpiryTest, ExpiresExactlyAfterTtlIdleEntries) {
  Replica replica(kTtl10);
  const std::uint64_t id = registered(replica, 1);
  EXPECT_EQ(result(replica, 11, one(id, 1, buy(1))), SessionResult::kApplied);
  EXPECT_EQ(result(replica, 21, one(id, 2, buy(2))), SessionResult::kApplied);
  EXPECT_EQ(result(replica, 32, one(id, 3, buy(3))), SessionResult::kUnknownSession);
}

TEST(SessionExpiryTest, ExpiryHappensOnApplyEvenWithoutTraffic) {
  Replica replica(kTtl10);
  const std::uint64_t empty_hash = replica.hash();
  registered(replica, 1);
  const std::uint64_t live_hash = replica.hash();
  EXPECT_TRUE(apply(replica, 11, {}).empty());
  EXPECT_EQ(replica.hash(), live_hash);
  EXPECT_TRUE(apply(replica, 12, Bytes(3)).empty());
  EXPECT_EQ(replica.hash(), empty_hash);
}

TEST(SessionExpiryTest, ExpiredRetryIsRejectedWithoutEffect) {
  Replica replica(ttl(5));
  const std::uint64_t id = registered(replica, 1);
  const Bytes order = one(id, 1, buy(1));
  EXPECT_EQ(result(replica, 2, order), SessionResult::kApplied);
  EXPECT_EQ(result(replica, 7, order), SessionResult::kDuplicate);
  EXPECT_TRUE(apply(replica, 8, {}).empty());
  const std::uint64_t hash = replica.hash();
  const std::uint64_t book = book_hash(replica.book());

  const std::vector<Outcome> retry = apply(replica, 9, order);
  EXPECT_EQ(retry,
            (std::vector<Outcome>{
                {.client = id, .seq = 1, .result = SessionResult::kUnknownSession, .events = {}}}));
  EXPECT_EQ(result(replica, 10, one(id, 2, buy(2))), SessionResult::kUnknownSession);
  EXPECT_EQ(replica.hash(), hash);
  EXPECT_EQ(book_hash(replica.book()), book);
  EXPECT_EQ(replica.book().size(), 1U);
}

TEST(SessionExpiryTest, ExpirySurvivesSnapshotAndRestore) {
  Replica original(kTtl10);
  const std::uint64_t first = registered(original, 1);
  const std::uint64_t second = registered(original, 5);
  EXPECT_TRUE(apply(original, 6, {}).empty());
  std::optional<Replica> restored = Replica::restore(original.snapshot(), kTtl10);
  if (!restored) {
    FAIL() << "valid snapshot rejected";
  }
  Replica& copy = *restored;
  EXPECT_EQ(copy.hash(), original.hash());

  for (Replica* replica : {&original, &copy}) {
    EXPECT_EQ(result(*replica, 11, one(first, 1, buy(1))), SessionResult::kApplied);
    EXPECT_EQ(result(*replica, 16, one(second, 1, buy(2))), SessionResult::kUnknownSession);
    EXPECT_EQ(result(*replica, 22, one(first, 2, buy(3))), SessionResult::kUnknownSession);
  }
  EXPECT_EQ(copy.hash(), original.hash());
  EXPECT_EQ(copy.snapshot(), original.snapshot());
}

TEST(SessionExpiryTest, RestoreRejectsSessionsAlreadyPastTtl) {
  Replica replica(kTtl10);
  registered(replica, 1);
  EXPECT_TRUE(apply(replica, 9, {}).empty());
  const Bytes bytes = replica.snapshot();
  EXPECT_TRUE(Replica::restore(bytes, kTtl10).has_value());
  EXPECT_TRUE(Replica::restore(bytes, ttl(8)).has_value());
  EXPECT_FALSE(Replica::restore(bytes, ttl(7)).has_value());
}

}  // namespace
}  // namespace rme::engine
