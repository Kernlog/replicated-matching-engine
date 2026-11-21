#include "engine/order_book.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace rme::engine {
namespace {

std::vector<std::uint64_t> ids(const OrderBook& book, Side side) {
  std::vector<std::uint64_t> out;
  book.for_each(side, [&](const RestingOrder& order) { out.push_back(order.id.value()); });
  return out;
}

class OrderBookTest : public ::testing::Test {
 protected:
  std::vector<Event> limit(std::uint64_t id, Side side, std::int64_t price, std::int64_t qty) {
    std::vector<Event> out;
    book_.apply(LimitOrder{OrderId(id), side, Price(price), Qty(qty)}, out);
    return out;
  }

  OrderBook book_;
};

TEST_F(OrderBookTest, EmptyBookHasNoBestPrices) {
  EXPECT_FALSE(book_.best_bid());
  EXPECT_FALSE(book_.best_ask());
  EXPECT_EQ(book_.size(), 0U);
}

TEST_F(OrderBookTest, NewOrderEmitsAccepted) {
  EXPECT_EQ(limit(1, Side::kBuy, 100, 10), std::vector<Event>{Accepted{OrderId(1)}});
}

TEST_F(OrderBookTest, BestBidIsHighestAndBestAskIsLowest) {
  limit(1, Side::kBuy, 99, 1);
  limit(2, Side::kBuy, 100, 1);
  limit(3, Side::kSell, 102, 1);
  limit(4, Side::kSell, 101, 1);

  EXPECT_EQ(book_.best_bid(), Price(100));
  EXPECT_EQ(book_.best_ask(), Price(101));
  EXPECT_EQ(book_.size(), 4U);
}

TEST_F(OrderBookTest, IteratesBestPriceFirstThenArrivalOrder) {
  limit(1, Side::kBuy, 99, 1);
  limit(2, Side::kBuy, 100, 1);
  limit(3, Side::kBuy, 99, 1);
  limit(4, Side::kBuy, 100, 1);

  EXPECT_EQ(ids(book_, Side::kBuy), (std::vector<std::uint64_t>{2, 4, 1, 3}));
}

}  // namespace
}  // namespace rme::engine
