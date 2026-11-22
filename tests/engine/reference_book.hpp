#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "engine/command.hpp"
#include "engine/event.hpp"
#include "engine/order_book.hpp"

namespace rme::engine {

class ReferenceBook {
 public:
  std::vector<Event> apply(const Command& command) {
    std::vector<Event> out;
    std::visit([this, &out](const auto& cmd) { on(cmd, out); }, command);
    return out;
  }

  [[nodiscard]] std::vector<RestingOrder> resting(Side side) const {
    std::vector<Entry> entries;
    std::copy_if(orders_.begin(), orders_.end(), std::back_inserter(entries),
                 [&](const Entry& entry) { return entry.side == side; });
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
      if (a.order.price != b.order.price) {
        return side == Side::kBuy ? a.order.price > b.order.price : a.order.price < b.order.price;
      }
      return a.seq < b.seq;
    });
    std::vector<RestingOrder> out;
    out.reserve(entries.size());
    for (const Entry& entry : entries) {
      out.push_back(entry.order);
    }
    return out;
  }

 private:
  struct Entry {
    RestingOrder order;
    Side side;
    std::uint64_t seq;
  };

  void on(const LimitOrder& order, std::vector<Event>& out) {
    if (const auto reason = reject(order.id, order.price, order.qty)) {
      out.emplace_back(Rejected{order.id, *reason});
      return;
    }
    out.emplace_back(Accepted{order.id});
    execute(order.id, order.side, order.price, order.qty, out);
  }

  void on(const MarketOrder& order, std::vector<Event>& out) {
    if (const auto reason = reject(order.id, std::nullopt, order.qty)) {
      out.emplace_back(Rejected{order.id, *reason});
      return;
    }
    out.emplace_back(Accepted{order.id});
    const Qty left = trade(order.id, order.side, std::nullopt, order.qty, out);
    if (left > Qty(0)) {
      out.emplace_back(Cancelled{order.id, left});
    }
  }

  void on(const Cancel& cancel, std::vector<Event>& out) {
    const auto found = find(cancel.id);
    if (found == orders_.end()) {
      out.emplace_back(Rejected{cancel.id, RejectReason::kUnknownOrder});
      return;
    }
    out.emplace_back(Cancelled{cancel.id, found->order.qty});
    orders_.erase(found);
  }

  void on(const Replace& replace, std::vector<Event>& out) {
    const auto found = find(replace.id);
    if (found == orders_.end()) {
      out.emplace_back(Rejected{replace.id, RejectReason::kUnknownOrder});
      return;
    }
    if (replace.qty <= Qty(0) || replace.price <= Price(0)) {
      out.emplace_back(Rejected{replace.id, replace.qty <= Qty(0) ? RejectReason::kInvalidQty
                                                                  : RejectReason::kInvalidPrice});
      return;
    }
    out.emplace_back(Replaced{replace.id, replace.price, replace.qty});
    if (replace.price == found->order.price && replace.qty <= found->order.qty) {
      found->order.qty = replace.qty;
      return;
    }
    const Side side = found->side;
    orders_.erase(found);
    execute(replace.id, side, replace.price, replace.qty, out);
  }

  [[nodiscard]] std::optional<RejectReason> reject(OrderId id, std::optional<Price> price,
                                                   Qty qty) const {
    if (qty <= Qty(0)) {
      return RejectReason::kInvalidQty;
    }
    if (price && *price <= Price(0)) {
      return RejectReason::kInvalidPrice;
    }
    if (std::any_of(orders_.begin(), orders_.end(),
                    [&](const Entry& entry) { return entry.order.id == id; })) {
      return RejectReason::kDuplicateId;
    }
    return std::nullopt;
  }

  void execute(OrderId id, Side side, Price price, Qty qty, std::vector<Event>& out) {
    const Qty left = trade(id, side, price, qty, out);
    if (left > Qty(0)) {
      orders_.push_back(Entry{RestingOrder{id, price, left}, side, next_seq_++});
    }
  }

  Qty trade(OrderId taker, Side side, std::optional<Price> limit, Qty qty,
            std::vector<Event>& out) {
    while (qty > Qty(0)) {
      const auto best = best_opposite(side);
      if (best == orders_.end()) {
        break;
      }
      const Price price = best->order.price;
      if (limit && (side == Side::kBuy ? price > *limit : price < *limit)) {
        break;
      }
      const Qty traded = std::min(qty, best->order.qty);
      out.emplace_back(Fill{taker, best->order.id, price, traded});
      qty -= traded;
      best->order.qty -= traded;
      if (best->order.qty == Qty(0)) {
        orders_.erase(best);
      }
    }
    return qty;
  }

  std::vector<Entry>::iterator best_opposite(Side side) {
    auto best = orders_.end();
    for (auto it = orders_.begin(); it != orders_.end(); ++it) {
      if (it->side == side) {
        continue;
      }
      if (best == orders_.end()) {
        best = it;
        continue;
      }
      const bool better = side == Side::kBuy ? it->order.price < best->order.price
                                             : it->order.price > best->order.price;
      const bool tie_earlier = it->order.price == best->order.price && it->seq < best->seq;
      if (better || tie_earlier) {
        best = it;
      }
    }
    return best;
  }

  std::vector<Entry>::iterator find(OrderId id) {
    return std::find_if(orders_.begin(), orders_.end(),
                        [&](const Entry& entry) { return entry.order.id == id; });
  }

  std::vector<Entry> orders_;
  std::uint64_t next_seq_ = 0;
};

}  // namespace rme::engine
