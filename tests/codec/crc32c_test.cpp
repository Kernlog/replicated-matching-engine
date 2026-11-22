#include "codec/crc32c.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/rng.hpp"

namespace rme::codec {
namespace {

template <std::size_t N>
constexpr std::array<std::byte, N> to_bytes(std::string_view text) {
  std::array<std::byte, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = static_cast<std::byte>(text[i]);
  }
  return out;
}

constexpr auto kCheck = to_bytes<9>("123456789");

static_assert(crc32c(kCheck) == 0xE3069283U);
static_assert(crc32c({}) == 0U);

std::array<std::byte, 32> filled(auto fn) {
  std::array<std::byte, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(fn(i));
  }
  return out;
}

TEST(Crc32cTest, MatchesRfc3720Vectors) {
  EXPECT_EQ(crc32c(kCheck), 0xE3069283U);
  EXPECT_EQ(crc32c(filled([](std::size_t) { return 0x00; })), 0x8A9136AAU);
  EXPECT_EQ(crc32c(filled([](std::size_t) { return 0xFF; })), 0x62A8AB43U);
  EXPECT_EQ(crc32c(filled([](std::size_t i) { return i; })), 0x46DD794EU);
  EXPECT_EQ(crc32c(filled([](std::size_t i) { return 31 - i; })), 0x113FDB5CU);
}

TEST(Crc32cTest, SeedChainsAcrossSplits) {
  const std::span<const std::byte> all(kCheck);
  for (std::size_t split = 0; split <= all.size(); ++split) {
    EXPECT_EQ(crc32c(all.subspan(split), crc32c(all.first(split))), crc32c(all)) << split;
  }
}

TEST(Crc32cTest, RuntimeMatchesTableAtEveryLength) {
  Rng rng(7);
  std::vector<std::byte> buffer(257);
  for (auto& b : buffer) {
    b = static_cast<std::byte>(rng.below(256));
  }
  for (std::size_t length = 0; length <= buffer.size(); ++length) {
    const std::span<const std::byte> data(buffer.data(), length);
    EXPECT_EQ(crc32c(data), ~detail::crc32c_table_update(~0U, data)) << length;
  }
}

}  // namespace
}  // namespace rme::codec
