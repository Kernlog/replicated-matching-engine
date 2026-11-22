#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/rng.hpp"
#include "core/types.hpp"
#include "engine/command.hpp"

namespace rme::testing {

struct StreamConfig {
  std::int64_t mid = 10'000;
  std::int64_t spread = 50;
  std::int64_t overlap = 5;
  std::int64_t max_qty = 100;
  std::size_t max_live = 1024;
};

class CommandStream {
 public:
  explicit CommandStream(std::uint64_t seed, StreamConfig config = {})
      : rng_(seed), config_(config) {
    live_.reserve(config_.max_live + 1);
  }

  engine::Command next() {
    if (live_.size() > config_.max_live) {
      return engine::Cancel{take_live()};
    }
    const std::uint64_t roll = rng_.below(100);
    if (roll < 2) {
      return invalid();
    }
    if (roll < 22) {
      return engine::Cancel{live_.empty() ? OrderId(0) : take_live()};
    }
    if (roll < 34) {
      return engine::Replace{any_live(), price(side()), qty()};
    }
    if (roll < 42) {
      return engine::MarketOrder{fresh(), side(), qty()};
    }
    const OrderId id = fresh();
    live_.push_back(id);
    const Side order_side = side();
    return engine::LimitOrder{id, order_side, price(order_side), qty()};
  }

 private:
  engine::Command invalid() {
    switch (rng_.below(3)) {
      case 0:
        return engine::LimitOrder{fresh(), side(), price(Side::kBuy), Qty(0)};
      case 1:
        return engine::LimitOrder{fresh(), side(), Price(0), qty()};
      default:
        return engine::LimitOrder{any_live(), side(), price(Side::kSell), qty()};
    }
  }

  OrderId fresh() noexcept { return OrderId(++last_id_); }

  OrderId any_live() noexcept {
    return live_.empty() ? OrderId(0) : live_[rng_.below(live_.size())];
  }

  OrderId take_live() noexcept {
    const std::size_t index = rng_.below(live_.size());
    const OrderId id = live_[index];
    live_[index] = live_.back();
    live_.pop_back();
    return id;
  }

  Side side() noexcept { return rng_.chance(1, 2) ? Side::kBuy : Side::kSell; }

  Price price(Side order_side) noexcept {
    const std::int64_t offset = rng_.between(-config_.overlap, config_.spread);
    return Price(order_side == Side::kBuy ? config_.mid - offset : config_.mid + offset);
  }

  Qty qty() noexcept { return Qty(rng_.between(1, config_.max_qty)); }

  Rng rng_;
  StreamConfig config_;
  std::uint64_t last_id_ = 0;
  std::vector<OrderId> live_;
};

}  // namespace rme::testing
