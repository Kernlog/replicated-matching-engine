#pragma once

#include <cstdint>

namespace rme {

class Hasher {
 public:
  constexpr Hasher() noexcept = default;
  // Resumes from a digest, so a snapshot can carry a running hash forward.
  constexpr explicit Hasher(std::uint64_t state) noexcept : state_(state) {}

  constexpr void add(std::uint64_t value) noexcept { state_ = mix(state_ ^ value); }

  [[nodiscard]] constexpr std::uint64_t digest() const noexcept { return state_; }

 private:
  static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15U;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9U;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBU;
    return x ^ (x >> 31U);
  }

  std::uint64_t state_ = 0;
};

}  // namespace rme
