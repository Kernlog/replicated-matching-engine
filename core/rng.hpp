#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace rme {

class Rng {
 public:
  constexpr explicit Rng(std::uint64_t seed) noexcept {
    for (auto& word : state_) {
      word = splitmix64(seed);
    }
  }

  constexpr std::uint64_t next() noexcept {
    const std::uint64_t result = std::rotl(state_[1] * 5, 7) * 9;
    const std::uint64_t shifted = state_[1] << 17U;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= shifted;
    state_[3] = std::rotl(state_[3], 45);
    return result;
  }

  constexpr std::uint64_t below(std::uint64_t bound) noexcept {
    const std::uint64_t threshold = (0 - bound) % bound;
    while (true) {
      const std::uint64_t value = next();
      if (value >= threshold) {
        return value % bound;
      }
    }
  }

  constexpr std::int64_t between(std::int64_t low, std::int64_t high) noexcept {
    const auto span = static_cast<std::uint64_t>(high - low) + 1;
    return low + static_cast<std::int64_t>(below(span));
  }

  constexpr bool chance(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    return below(denominator) < numerator;
  }

 private:
  static constexpr std::uint64_t splitmix64(std::uint64_t& x) noexcept {
    x += 0x9e3779b97f4a7c15U;
    std::uint64_t z = x;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9U;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebU;
    return z ^ (z >> 31U);
  }

  std::array<std::uint64_t, 4> state_{};
};

}  // namespace rme
