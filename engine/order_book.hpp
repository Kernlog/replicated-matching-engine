#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <vector>

#include "core/types.hpp"
#include "engine/command.hpp"
#include "engine/event.hpp"

namespace rme::engine {

struct RestingOrder {
  OrderId id;
  Price price;
  Qty qty;
};

class OrderBook {
 public:
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
  using Queue = std::list<RestingOrder>;
  using Bids = std::map<Price, Queue, std::greater<>>;
  using Asks = std::map<Price, Queue, std::less<>>;

  struct Locator {
    Side side;
    Queue::iterator it;
  };

  template <typename Levels, typename F>
  static void visit(const Levels& levels, F& fn) {
    for (const auto& [price, queue] : levels) {
      for (const auto& order : queue) {
        fn(order);
      }
    }
  }

  void on(const LimitOrder& order, std::vector<Event>& out);
  void rest(Side side, const RestingOrder& order);

  Bids bids_;
  Asks asks_;
  std::map<OrderId, Locator> index_;
};

}  // namespace rme::engine
