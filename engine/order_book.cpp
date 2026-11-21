#include "engine/order_book.hpp"

#include "core/assert.hpp"

namespace rme::engine {

void OrderBook::apply(const Command& command, std::vector<Event>& out) {
  std::visit([&](const auto& cmd) { on(cmd, out); }, command);
}

std::optional<Price> OrderBook::best_bid() const noexcept {
  return bids_.empty() ? std::nullopt : std::optional(bids_.begin()->first);
}

std::optional<Price> OrderBook::best_ask() const noexcept {
  return asks_.empty() ? std::nullopt : std::optional(asks_.begin()->first);
}

void OrderBook::on(const LimitOrder& order, std::vector<Event>& out) {
  out.emplace_back(Accepted{order.id});
  rest(order.side, RestingOrder{order.id, order.price, order.qty});
}

void OrderBook::rest(Side side, const RestingOrder& order) {
  auto& queue = side == Side::kBuy ? bids_[order.price] : asks_[order.price];
  queue.push_back(order);
  const bool inserted = index_.emplace(order.id, Locator{side, std::prev(queue.end())}).second;
  RME_ASSERT(inserted);
}

}  // namespace rme::engine
