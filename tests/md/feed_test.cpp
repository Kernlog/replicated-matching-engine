#include "md/feed.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace rme::md {
namespace {

std::vector<std::byte> records(std::size_t count) {
  std::vector<std::byte> out(count * codec::kClientRecordSize);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(i * 13);
  }
  return out;
}

TEST(Feed, SplitsEntriesIntoOneMessagePerRecordAndOneForAnEmptyEntry) {
  std::vector<Message> out;
  split_entry(10, 4, records(3), out);
  split_entry(13, 5, {}, out);
  ASSERT_EQ(out.size(), 4U);
  EXPECT_EQ(out[2].seq, 12U);
  EXPECT_EQ(out[2].position, 2U);
  EXPECT_EQ(out[2].count, 3U);
  EXPECT_EQ(out[3].index, 5U);
  EXPECT_EQ(out[3].count, 0U);
}

TEST(Feed, PacketsFitTheMtuRoundTripAndRejectCorruption) {
  std::vector<Message> messages;
  split_entry(1, 1, records(kMessagesPerPacket), messages);
  std::vector<std::byte> packet;
  encode_packet(messages, packet);
  EXPECT_LE(packet.size(), kMaxDatagram);
  const std::vector<Message> decoded = decode_packet(packet).value_or(std::vector<Message>{});
  ASSERT_EQ(decoded.size(), messages.size());
  EXPECT_EQ(decoded.back().record, messages.back().record);
  EXPECT_EQ(decoded.back().seq, messages.back().seq);
  packet[100] ^= std::byte{1};
  EXPECT_FALSE(decode_packet(packet).has_value());
  EXPECT_FALSE(decode_packet(std::span(packet).first(20)).has_value());
}

}  // namespace
}  // namespace rme::md
