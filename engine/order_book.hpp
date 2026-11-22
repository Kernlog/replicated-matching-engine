#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/types.hpp"
#include "engine/command.hpp"
#include "engine/event.hpp"
#include "engine/order_index.hpp"
#include "engine/order_pool.hpp"
#include "engine/price_ladder.hpp"

namespace rme::engine {

struct BookConfig {
  std::uint32_t max_orders = 1U << 16U;
  Price min_price = Price(1);
  std::uint32_t price_levels = 1U << 16U;
};

class OrderBook {
 public:
  explicit OrderBook(BookConfig config = {});

  void apply(const Command& command, std::vector<Event>& out);

  [[nodiscard]] std::optional<Price> best_bid() const noexcept;
  [[nodiscard]] std::optional<Price> best_ask() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }

  template <typename F>
  void for_each(Side side, F fn) const {
    const auto visit_level = [&](std::uint32_t slot) {
      const Level& level = side == Side::kBuy ? bids_[slot] : asks_[slot];
      for (std::uint32_t i = level.head; i != kNil; i = pool_[i].next) {
        const OrderNode& node = pool_[i];
        fn(RestingOrder{node.id, price_at(slot), node.qty});
      }
    };
    if (side == Side::kBuy) {
      bids_.for_each_descending(visit_level);
    } else {
      asks_.for_each_ascending(visit_level);
    }
  }

 private:
  void on(const LimitOrder& order, std::vector<Event>& out);
  void on(const MarketOrder& order, std::vector<Event>& out);
  void on(const Cancel& cancel, std::vector<Event>& out);
  void on(const Replace& replace, std::vector<Event>& out);
  void execute(const LimitOrder& order, std::vector<Event>& out);
  void rest(Side side, OrderId id, Price price, Qty qty);
  void remove(std::uint32_t index);
  [[nodiscard]] bool crossed() const noexcept;
  [[nodiscard]] bool in_band(Price price) const noexcept;
  [[nodiscard]] std::uint32_t slot(Price price) const noexcept;
  [[nodiscard]] Price price_at(std::uint32_t slot) const noexcept;
  [[nodiscard]] std::optional<RejectReason> invalid(std::optional<Price> price,
                                                    Qty qty) const noexcept;
  [[nodiscard]] std::optional<RejectReason> reject_reason(OrderId id, std::optional<Price> price,
                                                          Qty qty) const noexcept;

  template <typename Crosses>
  Qty match(Side taker_side, OrderId taker, Qty qty, Crosses crosses, std::vector<Event>& out);

  BookConfig config_;
  OrderPool pool_;
  OrderIndex index_;
  PriceLadder bids_;
  PriceLadder asks_;
};

}  // namespace rme::engine
