#pragma once

#include <cstdint>
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

enum class RejectReason : std::uint8_t {
  kUnknownOrder,
  kDuplicateId,
  kInvalidQty,
  kInvalidPrice,
  kBookFull,
};

struct Rejected {
  OrderId id;
  RejectReason reason;

  friend bool operator==(const Rejected&, const Rejected&) = default;
};

struct Replaced {
  OrderId id;
  Price price;
  Qty qty;

  friend bool operator==(const Replaced&, const Replaced&) = default;
};

using Event = std::variant<Accepted, Fill, Cancelled, Rejected, Replaced>;

}  // namespace rme::engine
