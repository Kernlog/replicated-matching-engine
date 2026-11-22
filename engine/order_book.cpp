#include "engine/order_book.hpp"

#include <algorithm>

#include "core/assert.hpp"

namespace rme::engine {

namespace {

std::optional<RejectReason> invalid(std::optional<Price> price, Qty qty) noexcept {
  if (qty <= Qty(0)) {
    return RejectReason::kInvalidQty;
  }
  if (price && *price <= Price(0)) {
    return RejectReason::kInvalidPrice;
  }
  return std::nullopt;
}

}  // namespace

OrderBook::OrderBook(BookConfig config) : pool_(config.max_orders) {}

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

std::optional<RejectReason> OrderBook::reject_reason(OrderId id, std::optional<Price> price,
                                                     Qty qty) const {
  if (const auto reason = invalid(price, qty)) {
    return reason;
  }
  if (index_.contains(id)) {
    return RejectReason::kDuplicateId;
  }
  if (price && pool_.full()) {
    return RejectReason::kBookFull;
  }
  return std::nullopt;
}

bool OrderBook::crossed() const noexcept {
  return !bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first;
}

template <typename Levels, typename Crosses>
Qty OrderBook::match(Levels& levels, OrderId taker, Qty qty, Crosses crosses,
                     std::vector<Event>& out) {
  while (qty > Qty(0) && !levels.empty() && crosses(levels.begin()->first)) {
    auto level = levels.begin();
    Level& queue = level->second;
    while (qty > Qty(0) && !queue.empty()) {
      const std::uint32_t head = queue.head;
      RestingOrder& maker = pool_[head].order;
      const Qty traded = std::min(qty, maker.qty);
      out.emplace_back(Fill{taker, maker.id, maker.price, traded});
      qty -= traded;
      maker.qty -= traded;
      if (maker.qty == Qty(0)) {
        queue.unlink(pool_, head);
        index_.erase(maker.id);
        pool_.release(head);
      }
    }
    if (queue.empty()) {
      levels.erase(level);
    }
  }
  return qty;
}

void OrderBook::on(const LimitOrder& order, std::vector<Event>& out) {
  if (const auto reason = reject_reason(order.id, order.price, order.qty)) {
    out.emplace_back(Rejected{order.id, *reason});
    return;
  }
  out.emplace_back(Accepted{order.id});
  execute(order, out);
}

void OrderBook::on(const Replace& replace, std::vector<Event>& out) {
  const auto found = index_.find(replace.id);
  if (found == index_.end()) {
    out.emplace_back(Rejected{replace.id, RejectReason::kUnknownOrder});
    return;
  }
  if (const auto reason = invalid(replace.price, replace.qty)) {
    out.emplace_back(Rejected{replace.id, *reason});
    return;
  }
  out.emplace_back(Replaced{replace.id, replace.price, replace.qty});
  OrderNode& node = pool_[found->second];
  if (replace.price == node.order.price && replace.qty <= node.order.qty) {
    node.order.qty = replace.qty;
    return;
  }
  const Side order_side = node.side;
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
  if (const auto reason = reject_reason(order.id, std::nullopt, order.qty)) {
    out.emplace_back(Rejected{order.id, *reason});
    return;
  }
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
void OrderBook::unlink(Levels& levels, std::uint32_t index) {
  const auto level = levels.find(pool_[index].order.price);
  RME_ASSERT(level != levels.end());
  level->second.unlink(pool_, index);
  if (level->second.empty()) {
    levels.erase(level);
  }
}

RestingOrder OrderBook::remove(Index::iterator found) {
  const std::uint32_t index = found->second;
  const OrderNode node = pool_[index];
  if (node.side == Side::kBuy) {
    unlink(bids_, index);
  } else {
    unlink(asks_, index);
  }
  index_.erase(found);
  pool_.release(index);
  return node.order;
}

void OrderBook::rest(Side side, const RestingOrder& order) {
  const std::uint32_t index = pool_.acquire();
  pool_[index].order = order;
  pool_[index].side = side;
  Level& level = side == Side::kBuy ? bids_[order.price] : asks_[order.price];
  level.push_back(pool_, index);
  const bool inserted = index_.emplace(order.id, index).second;
  RME_ASSERT(inserted);
}

}  // namespace rme::engine
