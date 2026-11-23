#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "codec/crc32c.hpp"

namespace rme::engine::wire {

static_assert(std::endian::native == std::endian::little, "snapshots are stored little-endian");

inline constexpr std::size_t kHeaderSize = 8;

class Writer {
 public:
  Writer(std::uint32_t version, std::size_t payload_size) {
    bytes_.reserve(kHeaderSize + payload_size);
    append(std::bit_cast<std::array<std::byte, 4>>(version));
    append(std::array<std::byte, 4>{});
  }

  void u64(std::uint64_t value) { append(std::bit_cast<std::array<std::byte, 8>>(value)); }
  void i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }
  void bytes(std::span<const std::byte> data) { append(data); }

  [[nodiscard]] std::vector<std::byte> seal() && {
    const std::uint32_t crc = codec::crc32c(std::span(bytes_).subspan(kHeaderSize));
    std::ranges::copy(std::bit_cast<std::array<std::byte, 4>>(crc), bytes_.begin() + 4);
    return std::move(bytes_);
  }

 private:
  void append(std::span<const std::byte> data) {
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }

  std::vector<std::byte> bytes_;
};

// Reads a sealed payload; any read past the end or a bad header clears ok().
class Reader {
 public:
  Reader(std::span<const std::byte> bytes, std::uint32_t version) noexcept {
    if (bytes.size() < kHeaderSize) {
      return;
    }
    rest_ = bytes.subspan(kHeaderSize);
    ok_ = word<std::uint32_t>(bytes.first(4)) == version &&
          word<std::uint32_t>(bytes.subspan(4, 4)) == codec::crc32c(rest_);
  }

  [[nodiscard]] std::uint64_t u64() noexcept {
    if (rest_.size() < sizeof(std::uint64_t)) {
      ok_ = false;
      return 0;
    }
    const auto value = word<std::uint64_t>(rest_.first(sizeof(std::uint64_t)));
    rest_ = rest_.subspan(sizeof(std::uint64_t));
    return value;
  }

  [[nodiscard]] std::int64_t i64() noexcept { return std::bit_cast<std::int64_t>(u64()); }

  [[nodiscard]] std::span<const std::byte> take_rest() noexcept { return std::exchange(rest_, {}); }

  [[nodiscard]] std::size_t remaining() const noexcept { return rest_.size(); }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

 private:
  template <typename T>
  static T word(std::span<const std::byte> bytes) noexcept {
    std::array<std::byte, sizeof(T)> raw{};
    std::ranges::copy(bytes, raw.begin());
    return std::bit_cast<T>(raw);
  }

  std::span<const std::byte> rest_;
  bool ok_ = false;
};

}  // namespace rme::engine::wire
