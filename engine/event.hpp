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

struct Cancelled {
  OrderId id;
  Qty qty;

  friend bool operator==(const Cancelled&, const Cancelled&) = default;
};

using Event = std::variant<Accepted, Fill, Cancelled>;

}  // namespace rme::engine
