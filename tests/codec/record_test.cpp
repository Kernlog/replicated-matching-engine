#include "codec/record.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "testing/command_stream.hpp"

namespace rme::codec {
namespace {

using engine::Cancel;
using engine::Command;
using engine::LimitOrder;
using engine::MarketOrder;
using engine::Replace;

constexpr Command kLimit = LimitOrder{OrderId(7), Side::kSell, Price(10'050), Qty(25)};

static_assert(decode(encode(kLimit)) == std::optional(kLimit));

constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

std::vector<Command> samples() {
  return {
      kLimit,
      LimitOrder{OrderId(std::numeric_limits<std::uint64_t>::max()), Side::kBuy, Price(kMin),
                 Qty(kMax)},
      MarketOrder{OrderId(1), Side::kBuy, Qty(-1)},
      Cancel{OrderId(0)},
      Replace{OrderId(9), Price(kMax), Qty(kMin)},
  };
}

RecordBytes sealed(CommandRecord record) {
  record.crc = detail::checksum(record);
  return std::bit_cast<RecordBytes>(record);
}

TEST(RecordTest, RoundTripsEveryCommandType) {
  for (const Command& command : samples()) {
    EXPECT_EQ(decode(encode(command)), std::optional(command));
  }
}

TEST(RecordTest, RoundTripsCommandStream) {
  testing::CommandStream stream(3);
  for (int i = 0; i < 100'000; ++i) {
    const Command command = stream.next();
    ASSERT_EQ(decode(encode(command)), std::optional(command)) << i;
  }
}

TEST(RecordTest, RejectsEverySingleBitFlip) {
  for (const Command& command : samples()) {
    const RecordBytes clean = encode(command);
    for (std::size_t bit = 0; bit < kRecordSize * 8; ++bit) {
      RecordBytes flipped = clean;
      flipped[bit / 8] ^= std::byte{1} << (bit % 8);
      EXPECT_EQ(decode(flipped), std::nullopt) << bit;
    }
  }
}

TEST(RecordTest, RejectsWrongSize) {
  const RecordBytes bytes = encode(kLimit);
  EXPECT_EQ(decode(std::span(bytes).first(kRecordSize - 1)), std::nullopt);
  EXPECT_EQ(decode({}), std::nullopt);
}

TEST(RecordTest, RejectsSealedButNonCanonicalRecords) {
  EXPECT_EQ(decode(sealed({.version = 2, .type = RecordType::kCancel})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType{0}})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType{5}})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType::kLimit, .side = 0})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType::kLimit, .side = 3})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType::kMarket, .side = 1, .price = 1})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType::kCancel, .side = 1})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType::kCancel, .price = 1})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType::kCancel, .qty = 1})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType::kReplace, .side = 1})), std::nullopt);
  EXPECT_EQ(decode(sealed({.type = RecordType::kCancel})), std::optional<Command>(Cancel{}));
}

}  // namespace
}  // namespace rme::codec
