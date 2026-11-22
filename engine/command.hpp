#pragma once

#include <variant>

#include "core/types.hpp"

namespace rme::engine {

struct LimitOrder {
  OrderId id;
  Side side;
  Price price;
  Qty qty;

  friend bool operator==(const LimitOrder&, const LimitOrder&) = default;
};

struct MarketOrder {
  OrderId id;
  Side side;
  Qty qty;

  friend bool operator==(const MarketOrder&, const MarketOrder&) = default;
};

struct Cancel {
  OrderId id;

  friend bool operator==(const Cancel&, const Cancel&) = default;
};

struct Replace {
  OrderId id;
  Price price;
  Qty qty;

  friend bool operator==(const Replace&, const Replace&) = default;
};

using Command = std::variant<LimitOrder, MarketOrder, Cancel, Replace>;

}  // namespace rme::engine
