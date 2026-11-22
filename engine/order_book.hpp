#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "core/types.hpp"
#include "engine/command.hpp"
#include "engine/event.hpp"
#include "engine/order_pool.hpp"

namespace rme::engine {

struct BookConfig {
  std::uint32_t max_orders = 1U << 16U;
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
    if (side == Side::kBuy) {
      visit(bids_, fn);
    } else {
      visit(asks_, fn);
    }
  }

 private:
  using Bids = std::map<Price, Level, std::greater<>>;
  using Asks = std::map<Price, Level, std::less<>>;
  using Index = std::map<OrderId, std::uint32_t>;

  template <typename Levels, typename F>
  void visit(const Levels& levels, F& fn) const {
    for (const auto& [price, level] : levels) {
      for (std::uint32_t i = level.head; i != kNil; i = pool_[i].next) {
        fn(pool_[i].order);
      }
    }
  }

  void on(const LimitOrder& order, std::vector<Event>& out);
  void on(const MarketOrder& order, std::vector<Event>& out);
  void on(const Cancel& cancel, std::vector<Event>& out);
  void on(const Replace& replace, std::vector<Event>& out);
  void execute(const LimitOrder& order, std::vector<Event>& out);
  void rest(Side side, const RestingOrder& order);
  RestingOrder remove(Index::iterator found);
  [[nodiscard]] bool crossed() const noexcept;
  [[nodiscard]] std::optional<RejectReason> reject_reason(OrderId id, std::optional<Price> price,
                                                          Qty qty) const;

  template <typename Levels>
  void unlink(Levels& levels, std::uint32_t index);

  template <typename Levels, typename Crosses>
  Qty match(Levels& levels, OrderId taker, Qty qty, Crosses crosses, std::vector<Event>& out);

  OrderPool pool_;
  Bids bids_;
  Asks asks_;
  Index index_;
};

}  // namespace rme::engine
