#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace rme::codec {

namespace detail {

inline constexpr std::uint32_t kCastagnoli = 0x82F63B78U;

consteval std::array<std::uint32_t, 256> crc32c_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? kCastagnoli : 0U);
    }
    table[i] = crc;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = crc32c_table();

constexpr std::uint32_t crc32c_table_update(std::uint32_t crc,
                                            std::span<const std::byte> data) noexcept {
  for (const std::byte b : data) {
    crc = (crc >> 8U) ^ kCrc32cTable[(crc ^ static_cast<std::uint32_t>(b)) & 0xFFU];
  }
  return crc;
}

#if defined(__SSE4_2__)
inline std::uint32_t crc32c_hw_update(std::uint32_t crc, std::span<const std::byte> data) noexcept {
  std::uint64_t wide = crc;
  std::size_t i = 0;
  for (; i + sizeof(std::uint64_t) <= data.size(); i += sizeof(std::uint64_t)) {
    std::uint64_t word = 0;
    std::memcpy(&word, data.subspan(i, sizeof word).data(), sizeof word);
    wide = _mm_crc32_u64(wide, word);
  }
  crc = static_cast<std::uint32_t>(wide);
  for (; i < data.size(); ++i) {
    crc = _mm_crc32_u8(crc, static_cast<std::uint8_t>(data[i]));
  }
  return crc;
}
#endif

}  // namespace detail

[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::byte> data,
                                             std::uint32_t seed = 0) noexcept {
#if defined(__SSE4_2__)
  if (!std::is_constant_evaluated()) {
    return ~detail::crc32c_hw_update(~seed, data);
  }
#endif
  return ~detail::crc32c_table_update(~seed, data);
}

}  // namespace rme::codec
