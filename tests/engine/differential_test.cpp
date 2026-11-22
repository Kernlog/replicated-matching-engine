#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <tuple>
#include <vector>

#include "engine/order_book.hpp"
#include "testing/command_stream.hpp"
#include "tests/engine/reference_book.hpp"

namespace rme::engine {
namespace {

std::uint64_t env_or(const char* name, std::uint64_t fallback) {
  const char* value = std::getenv(name);
  return value == nullptr ? fallback : std::stoull(value);
}

using Snapshot = std::vector<std::tuple<std::uint64_t, std::int64_t, std::int64_t>>;

template <typename Orders>
Snapshot snapshot(const Orders& orders) {
  Snapshot out;
  for (const RestingOrder& order : orders) {
    out.emplace_back(order.id.value(), order.price.value(), order.qty.value());
  }
  return out;
}

std::vector<RestingOrder> resting(const OrderBook& book, Side side) {
  std::vector<RestingOrder> out;
  book.for_each(side, [&](const RestingOrder& order) { out.push_back(order); });
  return out;
}

TEST(DifferentialTest, EventsMatchReferenceBook) {
  const std::uint64_t seed = env_or("RME_SEED", 1);
  const std::uint64_t commands = env_or("RME_FUZZ_COMMANDS", 1'000'000);
  testing::CommandStream stream(seed);
  OrderBook book;
  ReferenceBook reference;
  std::vector<Event> events;

  for (std::uint64_t i = 0; i < commands; ++i) {
    const Command command = stream.next();
    events.clear();
    book.apply(command, events);
    ASSERT_EQ(events, reference.apply(command)) << "RME_SEED=" << seed << " command " << i;
    if (i % 1024 == 0) {
      for (const Side side : {Side::kBuy, Side::kSell}) {
        ASSERT_EQ(snapshot(resting(book, side)), snapshot(reference.resting(side)))
            << "RME_SEED=" << seed << " command " << i;
      }
    }
  }
  RecordProperty("commands", std::to_string(commands));
  RecordProperty("resting", std::to_string(book.size()));
}

}  // namespace
}  // namespace rme::engine
