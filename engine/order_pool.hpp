#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "core/assert.hpp"
#include "core/types.hpp"

namespace rme::engine {

struct RestingOrder {
  OrderId id;
  Price price;
  Qty qty;
};

inline constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

struct OrderNode {
  RestingOrder order;
  std::uint32_t prev = kNil;
  std::uint32_t next = kNil;
  Side side = Side::kBuy;
};

class OrderPool {
 public:
  explicit OrderPool(std::uint32_t capacity) : nodes_(capacity) {
    for (std::uint32_t i = 0; i + 1 < capacity; ++i) {
      nodes_[i].next = i + 1;
    }
    free_ = capacity == 0 ? kNil : 0;
  }

  [[nodiscard]] bool full() const noexcept { return free_ == kNil; }

  std::uint32_t acquire() noexcept {
    RME_ASSERT(!full());
    const std::uint32_t index = free_;
    free_ = nodes_[index].next;
    nodes_[index].prev = kNil;
    nodes_[index].next = kNil;
    return index;
  }

  void release(std::uint32_t index) noexcept {
    nodes_[index].next = free_;
    free_ = index;
  }

  OrderNode& operator[](std::uint32_t index) noexcept { return nodes_[index]; }
  const OrderNode& operator[](std::uint32_t index) const noexcept { return nodes_[index]; }

 private:
  std::vector<OrderNode> nodes_;
  std::uint32_t free_ = kNil;
};

struct Level {
  std::uint32_t head = kNil;
  std::uint32_t tail = kNil;

  [[nodiscard]] bool empty() const noexcept { return head == kNil; }

  void push_back(OrderPool& pool, std::uint32_t index) noexcept {
    pool[index].prev = tail;
    pool[index].next = kNil;
    if (tail == kNil) {
      head = index;
    } else {
      pool[tail].next = index;
    }
    tail = index;
  }

  void unlink(OrderPool& pool, std::uint32_t index) noexcept {
    const OrderNode& node = pool[index];
    if (node.prev == kNil) {
      head = node.next;
    } else {
      pool[node.prev].next = node.next;
    }
    if (node.next == kNil) {
      tail = node.prev;
    } else {
      pool[node.next].prev = node.prev;
    }
  }
};

}  // namespace rme::engine
