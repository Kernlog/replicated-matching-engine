#pragma once

#include <cstdint>

#include "core/hash.hpp"
#include "core/types.hpp"
#include "engine/order_book.hpp"

namespace rme::engine {

[[nodiscard]] inline std::uint64_t book_hash(const OrderBook& book) {
  Hasher hasher;
  for (const Side side : {Side::kBuy, Side::kSell}) {
    std::uint64_t count = 0;
    book.for_each(side, [&](const RestingOrder& order) {
      hasher.add(order.id.value());
      hasher.add(static_cast<std::uint64_t>(order.price.value()));
      hasher.add(static_cast<std::uint64_t>(order.qty.value()));
      ++count;
    });
    hasher.add(count);
  }
  return hasher.digest();
}

}  // namespace rme::engine
