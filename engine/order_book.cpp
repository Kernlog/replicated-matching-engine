#include "engine/order_book.hpp"

#include <algorithm>

#include "core/assert.hpp"

namespace rme::engine {

OrderBook::OrderBook(BookConfig config)
    : config_(config),
      pool_(config.max_orders),
      index_(config.max_orders),
      bids_(config.price_levels),
      asks_(config.price_levels) {
  RME_ASSERT(config.min_price > Price(0));
}

void OrderBook::apply(const Command& command, std::vector<Event>& out) {
  std::visit([this, &out](const auto& cmd) { on(cmd, out); }, command);
  RME_ASSERT(!crossed());
}

std::optional<Price> OrderBook::best_bid() const noexcept {
  const auto best = bids_.highest();
  return best ? std::optional(price_at(*best)) : std::nullopt;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
  const auto best = asks_.lowest();
  return best ? std::optional(price_at(*best)) : std::nullopt;
}

bool OrderBook::in_band(Price price) const noexcept {
  return price >= config_.min_price &&
         price.value() - config_.min_price.value() < std::int64_t{config_.price_levels};
}

std::uint32_t OrderBook::slot(Price price) const noexcept {
  return static_cast<std::uint32_t>(price.value() - config_.min_price.value());
}

Price OrderBook::price_at(std::uint32_t slot) const noexcept {
  return config_.min_price + Price(std::int64_t{slot});
}

std::optional<RejectReason> OrderBook::invalid(std::optional<Price> price, Qty qty) const noexcept {
  if (qty <= Qty(0)) {
    return RejectReason::kInvalidQty;
  }
  if (price && !in_band(*price)) {
    return RejectReason::kInvalidPrice;
  }
  return std::nullopt;
}

std::optional<RejectReason> OrderBook::reject_reason(OrderId id, std::optional<Price> price,
                                                     Qty qty) const noexcept {
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
  const auto bid = bids_.highest();
  const auto ask = asks_.lowest();
  return bid && ask && *bid >= *ask;
}

template <typename Crosses>
Qty OrderBook::match(Side taker_side, OrderId taker, Qty qty, Crosses crosses,
                     std::vector<Event>& out) {
  PriceLadder& ladder = taker_side == Side::kBuy ? asks_ : bids_;
  while (qty > Qty(0)) {
    const auto best = taker_side == Side::kBuy ? ladder.lowest() : ladder.highest();
    if (!best || !crosses(price_at(*best))) {
      break;
    }
    Level& level = ladder[*best];
    while (qty > Qty(0) && !level.empty()) {
      const std::uint32_t head = level.head;
      RestingOrder& maker = pool_[head].order;
      const Qty traded = std::min(qty, maker.qty);
      out.emplace_back(Fill{taker, maker.id, maker.price, traded});
      qty -= traded;
      maker.qty -= traded;
      if (maker.qty == Qty(0)) {
        level.unlink(pool_, head);
        index_.erase(maker.id);
        pool_.release(head);
      }
    }
    if (level.empty()) {
      ladder.clear(*best);
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

void OrderBook::on(const MarketOrder& order, std::vector<Event>& out) {
  if (const auto reason = reject_reason(order.id, std::nullopt, order.qty)) {
    out.emplace_back(Rejected{order.id, *reason});
    return;
  }
  out.emplace_back(Accepted{order.id});
  const Qty remaining = match(order.side, order.id, order.qty, [](Price) { return true; }, out);
  if (remaining > Qty(0)) {
    out.emplace_back(Cancelled{order.id, remaining});
  }
}

void OrderBook::on(const Cancel& cancel, std::vector<Event>& out) {
  const std::uint32_t found = index_.find(cancel.id);
  if (found == kNil) {
    out.emplace_back(Rejected{cancel.id, RejectReason::kUnknownOrder});
    return;
  }
  const RestingOrder removed = remove(found);
  out.emplace_back(Cancelled{removed.id, removed.qty});
}

void OrderBook::on(const Replace& replace, std::vector<Event>& out) {
  const std::uint32_t found = index_.find(replace.id);
  if (found == kNil) {
    out.emplace_back(Rejected{replace.id, RejectReason::kUnknownOrder});
    return;
  }
  if (const auto reason = invalid(replace.price, replace.qty)) {
    out.emplace_back(Rejected{replace.id, *reason});
    return;
  }
  out.emplace_back(Replaced{replace.id, replace.price, replace.qty});
  OrderNode& node = pool_[found];
  if (replace.price == node.order.price && replace.qty <= node.order.qty) {
    node.order.qty = replace.qty;
    return;
  }
  const Side side = node.side;
  remove(found);
  execute(LimitOrder{replace.id, side, replace.price, replace.qty}, out);
}

void OrderBook::execute(const LimitOrder& order, std::vector<Event>& out) {
  const Qty remaining =
      order.side == Side::kBuy
          ? match(
                order.side, order.id, order.qty, [&](Price ask) { return ask <= order.price; }, out)
          : match(
                order.side, order.id, order.qty, [&](Price bid) { return bid >= order.price; },
                out);
  if (remaining > Qty(0)) {
    rest(order.side, RestingOrder{order.id, order.price, remaining});
  }
}

RestingOrder OrderBook::remove(std::uint32_t index) {
  const OrderNode node = pool_[index];
  PriceLadder& ladder = node.side == Side::kBuy ? bids_ : asks_;
  const std::uint32_t at = slot(node.order.price);
  Level& level = ladder[at];
  level.unlink(pool_, index);
  if (level.empty()) {
    ladder.clear(at);
  }
  index_.erase(node.order.id);
  pool_.release(index);
  return node.order;
}

void OrderBook::rest(Side side, const RestingOrder& order) {
  const std::uint32_t index = pool_.acquire();
  pool_[index].order = order;
  pool_[index].side = side;
  PriceLadder& ladder = side == Side::kBuy ? bids_ : asks_;
  const std::uint32_t at = slot(order.price);
  Level& level = ladder[at];
  if (level.empty()) {
    ladder.mark(at);
  }
  level.push_back(pool_, index);
  const bool inserted = index_.insert(order.id, index);
  RME_ASSERT(inserted);
}

}  // namespace rme::engine
