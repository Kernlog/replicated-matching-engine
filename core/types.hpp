#pragma once

#include <cstdint>
#include <type_traits>

#include "core/strong.hpp"

namespace rme {

struct PriceTag {
  static constexpr bool kAdditive = true;
};
struct QtyTag {
  static constexpr bool kAdditive = true;
};
struct OrderIdTag {
  static constexpr bool kAdditive = false;
};

using Price = Strong<std::int64_t, PriceTag>;
using Qty = Strong<std::int64_t, QtyTag>;
using OrderId = Strong<std::uint64_t, OrderIdTag>;

enum class Side : std::uint8_t { kBuy, kSell };

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
  return side == Side::kBuy ? Side::kSell : Side::kBuy;
}

static_assert(sizeof(Price) == sizeof(std::int64_t) && std::is_trivially_copyable_v<Price>);
static_assert(sizeof(Qty) == sizeof(std::int64_t) && std::is_trivially_copyable_v<Qty>);
static_assert(sizeof(OrderId) == sizeof(std::uint64_t) && std::is_trivially_copyable_v<OrderId>);

}  // namespace rme
