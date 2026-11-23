#include "engine/snapshot.hpp"

#include <cstdint>
#include <utility>

#include "engine/wire.hpp"

namespace rme::engine {

namespace {

constexpr std::uint32_t kSnapshotVersion = 1;
constexpr std::size_t kOrderSize = 3 * sizeof(std::uint64_t);

bool in_order(Side side, std::optional<Price> previous, Price price) noexcept {
  if (!previous) {
    return true;
  }
  return side == Side::kBuy ? price <= *previous : price >= *previous;
}

}  // namespace

std::vector<std::byte> snapshot(const OrderBook& book) {
  std::uint64_t bids = 0;
  book.for_each(Side::kBuy, [&](const RestingOrder&) { ++bids; });
  wire::Writer out(kSnapshotVersion, 2 * sizeof(std::uint64_t) + book.size() * kOrderSize);
  out.u64(bids);
  out.u64(book.size() - bids);
  for (const Side side : {Side::kBuy, Side::kSell}) {
    book.for_each(side, [&](const RestingOrder& order) {
      out.u64(order.id.value());
      out.i64(order.price.value());
      out.i64(order.qty.value());
    });
  }
  return std::move(out).seal();
}

std::optional<OrderBook> restore(std::span<const std::byte> bytes, BookConfig config) {
  wire::Reader in(bytes, kSnapshotVersion);
  const std::uint64_t bids = in.u64();
  const std::uint64_t asks = in.u64();
  if (!in.ok() || bids > config.max_orders || asks > config.max_orders - bids ||
      in.remaining() != (bids + asks) * kOrderSize) {
    return std::nullopt;
  }
  OrderBook book(config);
  std::vector<Event> events;
  for (const auto& [side, count] : {std::pair{Side::kBuy, bids}, std::pair{Side::kSell, asks}}) {
    std::optional<Price> previous;
    for (std::uint64_t i = 0; i < count; ++i) {
      const OrderId id(in.u64());
      const Price price(in.i64());
      const Qty qty(in.i64());
      if (!in_order(side, previous, price)) {
        return std::nullopt;
      }
      previous = price;
      events.clear();
      book.apply(LimitOrder{id, side, price, qty}, events);
      if (events.size() != 1 || events.front() != Event(Accepted{id})) {
        return std::nullopt;
      }
    }
  }
  return book;
}

}  // namespace rme::engine
