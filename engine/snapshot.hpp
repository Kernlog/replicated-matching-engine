#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "engine/order_book.hpp"

namespace rme::engine {

[[nodiscard]] std::vector<std::byte> snapshot(const OrderBook& book);

[[nodiscard]] std::optional<OrderBook> restore(std::span<const std::byte> bytes,
                                               BookConfig config = {});

}  // namespace rme::engine
