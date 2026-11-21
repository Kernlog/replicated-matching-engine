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

using Command = std::variant<LimitOrder>;

}  // namespace rme::engine
