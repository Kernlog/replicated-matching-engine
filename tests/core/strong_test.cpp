#include <gtest/gtest.h>

#include <type_traits>

#include "core/types.hpp"

namespace rme {
namespace {

template <typename A, typename B>
concept Addable = requires(A a, B b) { a + b; };

static_assert(!std::is_convertible_v<std::int64_t, Price>);
static_assert(!std::is_convertible_v<Price, std::int64_t>);
static_assert(!std::is_convertible_v<Price, Qty>);
static_assert(!Addable<Price, Qty>);
static_assert(!Addable<OrderId, OrderId>);
static_assert(Addable<Qty, Qty>);
static_assert(Price(3) + Price(4) == Price(7));
static_assert(Qty(2) < Qty(3));

TEST(StrongTest, DefaultsToZero) {
  EXPECT_EQ(Qty().value(), 0);
}

TEST(StrongTest, CompoundArithmeticUpdatesInPlace) {
  Qty qty(10);
  qty -= Qty(3);
  qty += Qty(1);
  EXPECT_EQ(qty, Qty(8));
}

TEST(StrongTest, OppositeSideFlips) {
  EXPECT_EQ(opposite(Side::kBuy), Side::kSell);
  EXPECT_EQ(opposite(Side::kSell), Side::kBuy);
}

}  // namespace
}  // namespace rme
