#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/types.hpp"
#include "engine/order_pool.hpp"

namespace rme::engine {

class OrderIndex {
 public:
  explicit OrderIndex(std::uint32_t max_entries)
      : slots_(std::bit_ceil(std::max<std::size_t>(16, std::size_t{max_entries} * 2))),
        mask_(slots_.size() - 1),
        shift_(64 - std::countr_zero(slots_.size())) {}

  [[nodiscard]] std::uint32_t find(OrderId id) const noexcept {
    for (std::size_t i = home(id.value());; i = (i + 1) & mask_) {
      const Slot& slot = slots_[i];
      if (slot.value == kNil || slot.key == id.value()) {
        return slot.value;
      }
    }
  }

  [[nodiscard]] bool contains(OrderId id) const noexcept { return find(id) != kNil; }

  bool insert(OrderId id, std::uint32_t value) noexcept {
    for (std::size_t i = home(id.value());; i = (i + 1) & mask_) {
      Slot& slot = slots_[i];
      if (slot.value == kNil) {
        slot = Slot{id.value(), value};
        ++size_;
        return true;
      }
      if (slot.key == id.value()) {
        return false;
      }
    }
  }

  void erase(OrderId id) noexcept {
    std::size_t hole = home(id.value());
    while (slots_[hole].value != kNil && slots_[hole].key != id.value()) {
      hole = (hole + 1) & mask_;
    }
    if (slots_[hole].value == kNil) {
      return;
    }
    for (std::size_t next = (hole + 1) & mask_; slots_[next].value != kNil;
         next = (next + 1) & mask_) {
      const std::size_t ideal = home(slots_[next].key);
      if (((next - ideal) & mask_) >= ((next - hole) & mask_)) {
        slots_[hole] = slots_[next];
        hole = next;
      }
    }
    slots_[hole] = Slot{};
    --size_;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  struct Slot {
    std::uint64_t key = 0;
    std::uint32_t value = kNil;
    std::uint32_t pad = 0;
  };

  static_assert(sizeof(Slot) == 16);

  [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15U) >> shift_);
  }

  std::vector<Slot> slots_;
  std::size_t mask_;
  int shift_;
  std::size_t size_ = 0;
};

}  // namespace rme::engine
