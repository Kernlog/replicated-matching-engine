#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#include "core/assert.hpp"

namespace rme {

inline constexpr std::size_t kCacheLine = 64;
// Adjacent-line prefetch pulls lines in pairs, so contended atomics sit 128 bytes apart.
inline constexpr std::size_t kFalseSharingRange = 128;

inline void cpu_relax() noexcept {
#if defined(__x86_64__)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

// Single-producer single-consumer ring. Each side owns its index on its own line and caches the
// other side's index, so a push or pop touches the shared line only when the cache runs out.
template <typename T>
  requires std::is_nothrow_move_assignable_v<T> && std::is_nothrow_default_constructible_v<T>
class SpscQueue {
 public:
  explicit SpscQueue(std::size_t capacity)
      : slots_(capacity), capacity_(capacity), mask_(capacity - 1) {
    RME_ASSERT(std::has_single_bit(capacity));
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;
  ~SpscQueue() = default;

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  bool try_push(T&& value) noexcept {
    const std::uint64_t tail = producer_.tail.load(std::memory_order_relaxed);
    if (tail - producer_.head_cache == capacity_) {
      producer_.head_cache = consumer_.head.load(std::memory_order_acquire);
      if (tail - producer_.head_cache == capacity_) {
        return false;
      }
    }
    slots_[tail & mask_] = std::move(value);
    publish(tail + 1);
    return true;
  }

  // Moves as many items as fit from the front of items and publishes them with one store.
  std::size_t push_n(std::span<T> items) noexcept {
    const std::uint64_t tail = producer_.tail.load(std::memory_order_relaxed);
    if (capacity_ - (tail - producer_.head_cache) < items.size()) {
      producer_.head_cache = consumer_.head.load(std::memory_order_acquire);
    }
    const std::size_t count =
        std::min<std::size_t>(items.size(), capacity_ - (tail - producer_.head_cache));
    for (std::size_t i = 0; i < count; ++i) {
      slots_[(tail + i) & mask_] = std::move(items[i]);
    }
    if (count != 0) {
      publish(tail + count);
    }
    return count;
  }

  // Spins while the ring is full; the consumer is expected to keep draining.
  void push(T&& value) noexcept {
    while (!try_push(std::move(value))) {
      cpu_relax();
    }
  }

  std::optional<T> try_pop() noexcept {
    const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
    if (head == consumer_.tail_cache) {
      consumer_.tail_cache = producer_.tail.load(std::memory_order_acquire);
      if (head == consumer_.tail_cache) {
        return std::nullopt;
      }
    }
    std::optional<T> value(std::move(slots_[head & mask_]));
    consumer_.head.store(head + 1, std::memory_order_release);
    return value;
  }

  // Moves up to out.size() items into out and frees their slots with one store.
  std::size_t pop_n(std::span<T> out) noexcept {
    const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
    if (consumer_.tail_cache - head < out.size()) {
      consumer_.tail_cache = producer_.tail.load(std::memory_order_acquire);
    }
    const std::size_t count = std::min<std::size_t>(out.size(), consumer_.tail_cache - head);
    for (std::size_t i = 0; i < count; ++i) {
      out[i] = std::move(slots_[(head + i) & mask_]);
    }
    if (count != 0) {
      consumer_.head.store(head + count, std::memory_order_release);
    }
    return count;
  }

  // Consumer side: returns once an item is ready. Spins first, then sleeps on a futex.
  void wait(std::size_t spins = 4096) noexcept {
    const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < spins; ++i) {
      if (producer_.tail.load(std::memory_order_acquire) != head) {
        return;
      }
      cpu_relax();
    }
    producer_.tail.wait(head, std::memory_order_acquire);
  }

 private:
  void publish(std::uint64_t tail) noexcept {
    producer_.tail.store(tail, std::memory_order_release);
    producer_.tail.notify_one();
  }

  struct alignas(kFalseSharingRange) Producer {
    std::atomic<std::uint64_t> tail{0};
    std::uint64_t head_cache = 0;
  };
  struct alignas(kFalseSharingRange) Consumer {
    std::atomic<std::uint64_t> head{0};
    std::uint64_t tail_cache = 0;
  };

  Producer producer_;
  Consumer consumer_;
  std::vector<T> slots_;
  std::size_t capacity_;
  std::uint64_t mask_;
};

}  // namespace rme
