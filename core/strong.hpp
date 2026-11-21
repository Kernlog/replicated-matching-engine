#pragma once

#include <compare>
#include <concepts>
#include <type_traits>

namespace rme {

template <typename Tag>
concept AdditiveTag = Tag::kAdditive;

template <std::integral T, typename Tag>
class Strong {
 public:
  using ValueType = T;

  constexpr Strong() noexcept = default;
  constexpr explicit Strong(T value) noexcept : value_(value) {}

  [[nodiscard]] constexpr T value() const noexcept { return value_; }

  friend constexpr auto operator<=>(Strong, Strong) noexcept = default;

  friend constexpr Strong operator+(Strong a, Strong b) noexcept
    requires AdditiveTag<Tag>
  {
    return Strong(static_cast<T>(a.value_ + b.value_));
  }

  friend constexpr Strong operator-(Strong a, Strong b) noexcept
    requires AdditiveTag<Tag>
  {
    return Strong(static_cast<T>(a.value_ - b.value_));
  }

  constexpr Strong& operator+=(Strong other) noexcept
    requires AdditiveTag<Tag>
  {
    value_ = static_cast<T>(value_ + other.value_);
    return *this;
  }

  constexpr Strong& operator-=(Strong other) noexcept
    requires AdditiveTag<Tag>
  {
    value_ = static_cast<T>(value_ - other.value_);
    return *this;
  }

 private:
  T value_{};
};

}  // namespace rme
