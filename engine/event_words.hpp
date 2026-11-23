#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "engine/event.hpp"

namespace rme::engine {

using EventWords = std::array<std::uint64_t, 5>;

namespace detail {

constexpr std::uint64_t word(std::int64_t value) noexcept {
  return std::bit_cast<std::uint64_t>(value);
}

constexpr std::int64_t signed_word(std::uint64_t value) noexcept {
  return std::bit_cast<std::int64_t>(value);
}

constexpr EventWords words_of(const Accepted& e) noexcept {
  return {0, e.id.value()};
}

constexpr EventWords words_of(const Fill& e) noexcept {
  return {1, e.taker.value(), e.maker.value(), word(e.price.value()), word(e.qty.value())};
}

constexpr EventWords words_of(const Cancelled& e) noexcept {
  return {2, e.id.value(), word(e.qty.value())};
}

constexpr EventWords words_of(const Rejected& e) noexcept {
  return {3, e.id.value(), static_cast<std::uint64_t>(e.reason)};
}

constexpr EventWords words_of(const Replaced& e) noexcept {
  return {4, e.id.value(), word(e.price.value()), word(e.qty.value())};
}

}  // namespace detail

[[nodiscard]] constexpr EventWords to_words(const Event& event) {
  return std::visit([](const auto& e) { return detail::words_of(e); }, event);
}

// Canonical inverse of to_words: unused trailing words must be zero.
[[nodiscard]] constexpr std::optional<Event> from_words(const EventWords& w) noexcept {
  const auto zero_from = [&](std::size_t first) {
    for (std::size_t i = first; i < w.size(); ++i) {
      if (w[i] != 0) {
        return false;
      }
    }
    return true;
  };
  const OrderId id(w[1]);
  switch (w[0]) {
    case 0:
      if (zero_from(2)) {
        return Accepted{id};
      }
      break;
    case 1:
      return Fill{id, OrderId(w[2]), Price(detail::signed_word(w[3])),
                  Qty(detail::signed_word(w[4]))};
    case 2:
      if (zero_from(3)) {
        return Cancelled{id, Qty(detail::signed_word(w[2]))};
      }
      break;
    case 3:
      if (zero_from(3) && w[2] <= static_cast<std::uint64_t>(RejectReason::kBookFull)) {
        return Rejected{id, static_cast<RejectReason>(w[2])};
      }
      break;
    case 4:
      if (zero_from(4)) {
        return Replaced{id, Price(detail::signed_word(w[2])), Qty(detail::signed_word(w[3]))};
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

}  // namespace rme::engine
