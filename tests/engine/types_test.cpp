#include <gtest/gtest.h>

#include <type_traits>

#include "engine/command.hpp"
#include "engine/event.hpp"

namespace rme::engine {
namespace {

static_assert(std::is_trivially_copyable_v<Command>);
static_assert(std::is_trivially_copyable_v<Event>);

TEST(EngineTypesTest, CommandsCompareByValue) {
  const Command a = LimitOrder{OrderId(1), Side::kBuy, Price(100), Qty(5)};
  const Command b = LimitOrder{OrderId(1), Side::kBuy, Price(100), Qty(5)};
  const Command c = LimitOrder{OrderId(1), Side::kBuy, Price(100), Qty(6)};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(EngineTypesTest, EventsCompareByAlternativeAndValue) {
  const Event accepted = Accepted{OrderId(1)};
  const Event fill = Fill{OrderId(1), OrderId(2), Price(100), Qty(5)};
  EXPECT_NE(accepted, fill);
  EXPECT_EQ(fill, Event(Fill{OrderId(1), OrderId(2), Price(100), Qty(5)}));
}

}  // namespace
}  // namespace rme::engine
