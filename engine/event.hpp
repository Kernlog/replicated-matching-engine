#pragma once

#include <variant>

#include "core/types.hpp"

namespace rme::engine {

struct Accepted {
  OrderId id;

  friend bool operator==(const Accepted&, const Accepted&) = default;
};

struct Fill {
  OrderId taker;
  OrderId maker;
  Price price;
  Qty qty;

  friend bool operator==(const Fill&, const Fill&) = default;
};

using Event = std::variant<Accepted, Fill>;

}  // namespace rme::engine
