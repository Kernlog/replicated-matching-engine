#include "engine/order_book.hpp"

#include <algorithm>

#include "core/assert.hpp"

namespace rme::engine {

void OrderBook::apply(const Command& command, std::vector<Event>& out) {
  std::visit([&](const auto& cmd) { on(cmd, out); }, command);
  RME_ASSERT(!crossed());
}

std::optional<Price> OrderBook::best_bid() const noexcept {
  return bids_.empty() ? std::nullopt : std::optional(bids_.begin()->first);
}

std::optional<Price> OrderBook::best_ask() const noexcept {
  return asks_.empty() ? std::nullopt : std::optional(asks_.begin()->first);
}

bool OrderBook::crossed() const noexcept {
  return !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first;
}

template <typename Levels, typename Crosses>
Qty OrderBook::match(Levels& levels, OrderId taker, Qty qty, Crosses crosses,
                     std::vector<Event>& out) {
  while (qty > Qty(0) && !levels.empty() && crosses(levels.begin()->first)) {
    auto level = levels.begin();
    auto& queue = level->second;
    while (qty > Qty(0) && !queue.empty()) {
      auto& maker = queue.front();
      const Qty traded = std::min(qty, maker.qty);
      out.emplace_back(Fill{taker, maker.id, maker.price, traded});
      qty -= traded;
      maker.qty -= traded;
      if (maker.qty == Qty(0)) {
        index_.erase(maker.id);
        queue.pop_front();
      }
    }
    if (queue.empty()) {
      levels.erase(level);
    }
  }
  return qty;
}

void OrderBook::on(const LimitOrder& order, std::vector<Event>& out) {
  out.emplace_back(Accepted{order.id});
  execute(order, out);
}

void OrderBook::on(const Replace& replace, std::vector<Event>& out) {
  const auto found = index_.find(replace.id);
  if (found == index_.end()) {
    out.emplace_back(Rejected{replace.id, RejectReason::kUnknownOrder});
    return;
  }
  out.emplace_back(Replaced{replace.id, replace.price, replace.qty});
  auto& [side, it] = found->second;
  if (replace.price == it->price && replace.qty <= it->qty) {
    it->qty = replace.qty;
    return;
  }
  const Side order_side = side;
  remove(found);
  execute(LimitOrder{replace.id, order_side, replace.price, replace.qty}, out);
}

void OrderBook::execute(const LimitOrder& order, std::vector<Event>& out) {
  const Qty remaining =
      order.side == Side::kBuy
          ? match(
                asks_, order.id, order.qty, [&](Price ask) { return ask <= order.price; }, out)
          : match(bids_, order.id, order.qty, [&](Price bid) { return bid >= order.price; }, out);
  if (remaining > Qty(0)) {
    rest(order.side, RestingOrder{order.id, order.price, remaining});
  }
}

void OrderBook::on(const MarketOrder& order, std::vector<Event>& out) {
  out.emplace_back(Accepted{order.id});
  const auto any_price = [](Price) { return true; };
  const Qty remaining = order.side == Side::kBuy
                            ? match(asks_, order.id, order.qty, any_price, out)
                            : match(bids_, order.id, order.qty, any_price, out);
  if (remaining > Qty(0)) {
    out.emplace_back(Cancelled{order.id, remaining});
  }
}

void OrderBook::on(const Cancel& cancel, std::vector<Event>& out) {
  const auto found = index_.find(cancel.id);
  if (found == index_.end()) {
    out.emplace_back(Rejected{cancel.id, RejectReason::kUnknownOrder});
    return;
  }
  const RestingOrder removed = remove(found);
  out.emplace_back(Cancelled{removed.id, removed.qty});
}

template <typename Levels>
void OrderBook::unlink(Levels& levels, Queue::iterator it) {
  const auto level = levels.find(it->price);
  RME_ASSERT(level != levels.end());
  level->second.erase(it);
  if (level->second.empty()) {
    levels.erase(level);
  }
}

RestingOrder OrderBook::remove(std::map<OrderId, Locator>::iterator found) {
  const auto [side, it] = found->second;
  const RestingOrder order = *it;
  if (side == Side::kBuy) {
    unlink(bids_, it);
  } else {
    unlink(asks_, it);
  }
  index_.erase(found);
  return order;
}

void OrderBook::rest(Side side, const RestingOrder& order) {
  auto& queue = side == Side::kBuy ? bids_[order.price] : asks_[order.price];
  queue.push_back(order);
  const bool inserted = index_.emplace(order.id, Locator{side, std::prev(queue.end())}).second;
  RME_ASSERT(inserted);
}

}  // namespace rme::engine
