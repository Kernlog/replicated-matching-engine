#include "engine/book_hash.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

namespace rme::engine {
namespace {

std::uint64_t hash_after(std::initializer_list<Command> commands) {
  OrderBook book;
  std::vector<Event> events;
  for (const Command& command : commands) {
    book.apply(command, events);
  }
  return book_hash(book);
}

LimitOrder buy(std::uint64_t id, std::int64_t price, std::int64_t qty) {
  return {OrderId(id), Side::kBuy, Price(price), Qty(qty)};
}

LimitOrder sell(std::uint64_t id, std::int64_t price, std::int64_t qty) {
  return {OrderId(id), Side::kSell, Price(price), Qty(qty)};
}

TEST(BookHashTest, EqualBooksHashEqualAcrossCommandPaths) {
  const std::uint64_t direct = hash_after({buy(1, 100, 10), sell(2, 105, 5)});
  EXPECT_EQ(hash_after({buy(1, 100, 30), sell(3, 99, 20), sell(2, 105, 5)}), direct);
  EXPECT_EQ(
      hash_after({buy(1, 100, 15), Replace{OrderId(1), Price(100), Qty(10)}, sell(2, 105, 5)}),
      direct);
  EXPECT_EQ(hash_after({buy(1, 100, 10), buy(4, 98, 1), sell(2, 105, 5), Cancel{OrderId(4)}}),
            direct);
  EXPECT_EQ(hash_after({}), hash_after({buy(1, 100, 10), Cancel{OrderId(1)}}));
}

TEST(BookHashTest, DiffersOnQueueOrder) {
  EXPECT_NE(hash_after({buy(1, 100, 10), buy(2, 100, 10)}),
            hash_after({buy(2, 100, 10), buy(1, 100, 10)}));
}

TEST(BookHashTest, DiffersOnSidePriceAndQty) {
  const std::uint64_t base = hash_after({buy(1, 100, 10)});
  EXPECT_NE(hash_after({sell(1, 100, 10)}), base);
  EXPECT_NE(hash_after({buy(1, 101, 10)}), base);
  EXPECT_NE(hash_after({buy(1, 100, 11)}), base);
  EXPECT_NE(hash_after({}), base);
}

}  // namespace
}  // namespace rme::engine
