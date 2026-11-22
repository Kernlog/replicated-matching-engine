#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/assert.hpp"
#include "engine/order_pool.hpp"

namespace rme::engine {

class PriceLadder {
 public:
  explicit PriceLadder(std::uint32_t levels)
      : levels_(levels), leaves_(levels / kBits), summary_((levels / kBits + kBits - 1) / kBits) {
    RME_ASSERT(levels > 0 && levels % kBits == 0);
  }

  Level& operator[](std::uint32_t slot) noexcept { return levels_[slot]; }
  const Level& operator[](std::uint32_t slot) const noexcept { return levels_[slot]; }

  void mark(std::uint32_t slot) noexcept {
    const std::uint32_t leaf = slot / kBits;
    leaves_[leaf] |= bit(slot % kBits);
    summary_[leaf / kBits] |= bit(leaf % kBits);
  }

  void clear(std::uint32_t slot) noexcept {
    const std::uint32_t leaf = slot / kBits;
    leaves_[leaf] &= ~bit(slot % kBits);
    if (leaves_[leaf] == 0) {
      summary_[leaf / kBits] &= ~bit(leaf % kBits);
    }
  }

  [[nodiscard]] std::optional<std::uint32_t> lowest() const noexcept {
    for (std::uint32_t s = 0; s < summary_.size(); ++s) {
      if (summary_[s] != 0) {
        const std::uint32_t leaf = s * kBits + low_bit(summary_[s]);
        return leaf * kBits + low_bit(leaves_[leaf]);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::uint32_t> highest() const noexcept {
    for (auto s = static_cast<std::uint32_t>(summary_.size()); s-- > 0;) {
      if (summary_[s] != 0) {
        const std::uint32_t leaf = s * kBits + high_bit(summary_[s]);
        return leaf * kBits + high_bit(leaves_[leaf]);
      }
    }
    return std::nullopt;
  }

  template <typename F>
  void for_each_ascending(F& fn) const {
    for (std::uint32_t leaf = 0; leaf < leaves_.size(); ++leaf) {
      for (std::uint64_t word = leaves_[leaf]; word != 0; word &= word - 1) {
        fn(leaf * kBits + low_bit(word));
      }
    }
  }

  template <typename F>
  void for_each_descending(F& fn) const {
    for (auto leaf = static_cast<std::uint32_t>(leaves_.size()); leaf-- > 0;) {
      for (std::uint64_t word = leaves_[leaf]; word != 0; word &= ~bit(high_bit(word))) {
        fn(leaf * kBits + high_bit(word));
      }
    }
  }

 private:
  static constexpr std::uint32_t kBits = 64;

  static constexpr std::uint64_t bit(std::uint32_t index) noexcept {
    return std::uint64_t{1} << index;
  }
  static constexpr std::uint32_t low_bit(std::uint64_t word) noexcept {
    return static_cast<std::uint32_t>(std::countr_zero(word));
  }
  static constexpr std::uint32_t high_bit(std::uint64_t word) noexcept {
    return kBits - 1 - static_cast<std::uint32_t>(std::countl_zero(word));
  }

  std::vector<Level> levels_;
  std::vector<std::uint64_t> leaves_;
  std::vector<std::uint64_t> summary_;
};

}  // namespace rme::engine
