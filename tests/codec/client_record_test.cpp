#include "codec/client_record.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "codec/batch.hpp"
#include "testing/command_stream.hpp"

namespace rme::codec {
namespace {

using engine::ClientCommand;

constexpr ClientCommand kLimit{
    .client = 3, .seq = 9, .command = engine::LimitOrder{OrderId(7), Side::kBuy, Price(5), Qty(2)}};

static_assert(decode_client(encode(kLimit)) == std::optional(kLimit));
static_assert(decode_client(encode_register()) == std::optional(ClientCommand{}));

TEST(ClientRecordTest, RoundTripsCommandStream) {
  testing::CommandStream stream(8);
  for (std::uint64_t i = 1; i < 50'000; ++i) {
    const ClientCommand command{.client = i * 7919, .seq = i, .command = stream.next()};
    ASSERT_EQ(decode_client(encode(command)), std::optional(command)) << i;
  }
}

TEST(ClientRecordTest, RejectsEverySingleBitFlip) {
  for (const ClientCommand& command :
       {kLimit, ClientCommand{},
        ClientCommand{.client = std::numeric_limits<std::uint64_t>::max(), .seq = 1}}) {
    const ClientRecordBytes clean = encode(command);
    for (std::size_t bit = 0; bit < kClientRecordSize * 8; ++bit) {
      ClientRecordBytes flipped = clean;
      flipped[bit / 8] ^= std::byte{1} << (bit % 8);
      EXPECT_EQ(decode_client(flipped), std::nullopt) << bit;
    }
  }
}

TEST(ClientRecordTest, RegisterRequestIsCanonical) {
  EXPECT_EQ(decode_client(encode(ClientCommand{.seq = 1})), std::nullopt);
  EXPECT_EQ(decode_client(encode(ClientCommand{.command = engine::Cancel{OrderId(1)}})),
            std::nullopt);
  const ClientRecordBytes bytes = encode_register();
  EXPECT_EQ(decode_client(std::span(bytes).first(kClientRecordSize - 1)), std::nullopt);
}

TEST(ClientRecordTest, BatchVisitsNothingUnlessEveryRecordDecodes) {
  std::vector<std::byte> batch;
  for (const ClientRecordBytes& record : {encode(kLimit), encode_register(), encode(kLimit)}) {
    batch.insert(batch.end(), record.begin(), record.end());
  }
  std::vector<ClientCommand> seen;
  EXPECT_TRUE(for_each_client_command(batch, [&](const ClientCommand& c) { seen.push_back(c); }));
  EXPECT_EQ(seen, (std::vector<ClientCommand>{kLimit, ClientCommand{}, kLimit}));

  seen.clear();
  batch[kClientRecordSize + 3] ^= std::byte{4};
  EXPECT_FALSE(for_each_client_command(batch, [&](const ClientCommand& c) { seen.push_back(c); }));
  batch.pop_back();
  EXPECT_FALSE(for_each_client_command(batch, [&](const ClientCommand& c) { seen.push_back(c); }));
  EXPECT_TRUE(seen.empty());
}

}  // namespace
}  // namespace rme::codec
