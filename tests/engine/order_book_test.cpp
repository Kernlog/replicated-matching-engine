#include "engine/order_book.hpp"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

namespace rme::engine {
namespace {

using Resting = std::vector<std::pair<std::uint64_t, std::int64_t>>;

Resting resting(const OrderBook& book, Side side) {
  Resting out;
  book.for_each(side, [&](const RestingOrder& order) {
    out.emplace_back(order.id.value(), order.qty.value());
  });
  return out;
}

Fill fill(std::uint64_t taker, std::uint64_t maker, std::int64_t price, std::int64_t qty) {
  return Fill{OrderId(taker), OrderId(maker), Price(price), Qty(qty)};
}

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

TEST_F(OrderBookTest, NonCrossingOrdersDoNotTrade) {
  limit(1, Side::kBuy, 100, 5);
  EXPECT_EQ(limit(2, Side::kSell, 101, 5), std::vector<Event>{Accepted{OrderId(2)}});
  EXPECT_EQ(book_.size(), 2U);
}

TEST_F(OrderBookTest, CrossingBuyFillsAtMakerPrice) {
  limit(1, Side::kSell, 101, 5);
  EXPECT_EQ(limit(2, Side::kBuy, 103, 5),
            (std::vector<Event>{Accepted{OrderId(2)}, fill(2, 1, 101, 5)}));
  EXPECT_EQ(book_.size(), 0U);
  EXPECT_FALSE(book_.best_ask());
}

TEST_F(OrderBookTest, FillsSamePriceInArrivalOrder) {
  limit(1, Side::kSell, 101, 3);
  limit(2, Side::kSell, 101, 3);
  EXPECT_EQ(limit(3, Side::kBuy, 101, 4),
            (std::vector<Event>{Accepted{OrderId(3)}, fill(3, 1, 101, 3), fill(3, 2, 101, 1)}));
  EXPECT_EQ(resting(book_, Side::kSell), (Resting{{2, 2}}));
}

TEST_F(OrderBookTest, AggressorSweepsLevelsAndRestsRemainder) {
  limit(1, Side::kSell, 101, 2);
  limit(2, Side::kSell, 102, 2);
  limit(3, Side::kSell, 103, 2);
  EXPECT_EQ(limit(4, Side::kBuy, 102, 5),
            (std::vector<Event>{Accepted{OrderId(4)}, fill(4, 1, 101, 2), fill(4, 2, 102, 2)}));
  EXPECT_EQ(resting(book_, Side::kBuy), (Resting{{4, 1}}));
  EXPECT_EQ(resting(book_, Side::kSell), (Resting{{3, 2}}));
  EXPECT_EQ(book_.best_bid(), Price(102));
  EXPECT_EQ(book_.best_ask(), Price(103));
}

TEST_F(OrderBookTest, SellAggressorMatchesHighestBidFirst) {
  limit(1, Side::kBuy, 99, 2);
  limit(2, Side::kBuy, 100, 2);
  EXPECT_EQ(limit(3, Side::kSell, 99, 3),
            (std::vector<Event>{Accepted{OrderId(3)}, fill(3, 2, 100, 2), fill(3, 1, 99, 1)}));
  EXPECT_EQ(resting(book_, Side::kBuy), (Resting{{1, 1}}));
}

TEST_F(OrderBookTest, PartialFillLeavesMakerQuantity) {
  limit(1, Side::kBuy, 100, 10);
  EXPECT_EQ(limit(2, Side::kSell, 100, 4),
            (std::vector<Event>{Accepted{OrderId(2)}, fill(2, 1, 100, 4)}));
  EXPECT_EQ(resting(book_, Side::kBuy), (Resting{{1, 6}}));
  EXPECT_EQ(book_.size(), 1U);
}

TEST_F(OrderBookTest, SellAggressorStopsAtLimitAndRestsRemainder) {
  limit(1, Side::kBuy, 101, 2);
  limit(2, Side::kBuy, 99, 2);
  EXPECT_EQ(limit(3, Side::kSell, 100, 5),
            (std::vector<Event>{Accepted{OrderId(3)}, fill(3, 1, 101, 2)}));
  EXPECT_EQ(resting(book_, Side::kSell), (Resting{{3, 3}}));
  EXPECT_EQ(resting(book_, Side::kBuy), (Resting{{2, 2}}));
  EXPECT_EQ(book_.best_ask(), Price(100));
}

}  // namespace
}  // namespace rme::engine
