#include "engine/snapshot.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/book_hash.hpp"
#include "engine/wire.hpp"
#include "testing/command_stream.hpp"

namespace rme::engine {
namespace {

std::vector<Command> commands(std::uint64_t seed, std::size_t count) {
  testing::CommandStream stream(seed);
  std::vector<Command> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(stream.next());
  }
  return out;
}

std::vector<Event> apply_all(OrderBook& book, std::span<const Command> commands) {
  std::vector<Event> events;
  for (const Command& command : commands) {
    book.apply(command, events);
  }
  return events;
}

OrderBook built(std::span<const Command> commands) {
  OrderBook book;
  apply_all(book, commands);
  return book;
}

OrderBook round_trip(const OrderBook& book) {
  std::optional<OrderBook> copy = restore(snapshot(book));
  if (!copy) {
    ADD_FAILURE() << "valid snapshot rejected";
    return OrderBook{};
  }
  return std::move(*copy);
}

struct Order {
  std::uint64_t id = 0;
  std::int64_t price = 0;
  std::int64_t qty = 0;
};

std::vector<std::byte> sealed(const std::vector<Order>& bids, const std::vector<Order>& asks) {
  wire::Writer out(1, 0);
  out.u64(bids.size());
  out.u64(asks.size());
  for (const auto* side : {&bids, &asks}) {
    for (const Order& order : *side) {
      out.u64(order.id);
      out.i64(order.price);
      out.i64(order.qty);
    }
  }
  return std::move(out).seal();
}

TEST(SnapshotTest, RestoredHashEqualsOriginalAcrossStreamPrefixes) {
  for (std::uint64_t seed = 1; seed <= 4; ++seed) {
    const std::vector<Command> stream = commands(seed, 20'000);
    OrderBook book;
    std::vector<Event> events;
    for (std::size_t i = 0; i < stream.size(); ++i) {
      book.apply(stream[i], events);
      if (i % 257 != 0) {
        continue;
      }
      const OrderBook restored = round_trip(book);
      ASSERT_EQ(book_hash(restored), book_hash(book)) << "seed " << seed << " command " << i;
      EXPECT_EQ(restored.size(), book.size());
      EXPECT_EQ(snapshot(restored), snapshot(book));
    }
  }
}

TEST(SnapshotTest, RestoredBookEmitsIdenticalEventsAfterwards) {
  const std::vector<Command> stream = commands(11, 30'000);
  for (const std::size_t prefix : std::array<std::size_t, 5>{0, 1, 500, 5'000, 15'000}) {
    const std::span<const Command> head = std::span(stream).first(prefix);
    const std::span<const Command> tail = std::span(stream).subspan(prefix);
    OrderBook original = built(head);
    OrderBook restored = round_trip(original);
    EXPECT_EQ(apply_all(restored, tail), apply_all(original, tail)) << prefix;
    EXPECT_EQ(book_hash(restored), book_hash(original)) << prefix;
  }
}

TEST(SnapshotTest, RestoresQueuePositionWithinALevel) {
  OrderBook book;
  apply_all(book, std::vector<Command>{LimitOrder{OrderId(2), Side::kBuy, Price(100), Qty(5)},
                                       LimitOrder{OrderId(1), Side::kBuy, Price(100), Qty(5)}});
  OrderBook restored = round_trip(book);
  std::vector<Event> fills;
  restored.apply(MarketOrder{OrderId(3), Side::kSell, Qty(5)}, fills);
  EXPECT_EQ(fills, (std::vector<Event>{Accepted{OrderId(3)},
                                       Fill{OrderId(3), OrderId(2), Price(100), Qty(5)}}));
}

TEST(SnapshotTest, RejectsEveryCorruptByteAndTruncation) {
  const std::vector<std::byte> clean = snapshot(built(commands(5, 200)));
  ASSERT_GT(clean.size(), wire::kHeaderSize + 16);
  for (std::size_t i = 0; i < clean.size(); ++i) {
    std::vector<std::byte> corrupt = clean;
    corrupt[i] ^= std::byte{0x01};
    EXPECT_FALSE(restore(corrupt).has_value()) << "byte " << i;
    EXPECT_FALSE(restore(std::span(clean).first(i)).has_value()) << "length " << i;
  }
  std::vector<std::byte> extended = clean;
  extended.push_back(std::byte{0});
  EXPECT_FALSE(restore(extended).has_value());
}

TEST(SnapshotTest, RejectsSealedButInvalidBooks) {
  EXPECT_TRUE(restore(sealed({{1, 100, 5}, {2, 99, 5}}, {{3, 101, 5}})).has_value());
  EXPECT_FALSE(restore(sealed({{1, 99, 5}, {2, 100, 5}}, {})).has_value());
  EXPECT_FALSE(restore(sealed({}, {{1, 101, 5}, {2, 100, 5}})).has_value());
  EXPECT_FALSE(restore(sealed({{1, 101, 5}}, {{2, 101, 5}})).has_value());
  EXPECT_FALSE(restore(sealed({{1, 100, 5}}, {{1, 101, 5}})).has_value());
  EXPECT_FALSE(restore(sealed({{1, 100, 0}}, {})).has_value());
  EXPECT_FALSE(restore(sealed({{1, 0, 5}}, {})).has_value());
  EXPECT_FALSE(restore(sealed({{1, 100, 5}, {2, 99, 5}}, {}), {.max_orders = 1}).has_value());
}

}  // namespace
}  // namespace rme::engine
