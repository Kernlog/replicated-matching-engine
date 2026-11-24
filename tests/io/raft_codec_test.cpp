#include "io/raft_codec.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace rme::io {
namespace {

using namespace consensus;

std::vector<Message> samples() {
  const Entry a{Term(3), Index(10), EntryType::kCommand, {std::byte{1}, std::byte{2}}};
  const Entry b{Term(3), Index(11), EntryType::kNoop, {}};
  return {
      Message{NodeId(1), NodeId(2), Term(3), VoteRequest{Index(9), Term(2), true}},
      Message{NodeId(2), NodeId(1), Term(3), VoteResponse{true, false}},
      Message{NodeId(1), NodeId(3), Term(3), AppendRequest{Index(9), Term(2), {a, b}, Index(8)}},
      Message{NodeId(3), NodeId(1), Term(3),
              AppendResponse{false, Index(4), Index(5), Term(1), true}},
      Message{NodeId(1), NodeId(2), Term(4),
              InstallSnapshot{Snapshot{Index(100), Term(4), std::vector<std::byte>(300)}}},
  };
}

TEST(RaftCodec, RoundTripsEveryMessageKind) {
  for (const Message& message : samples()) {
    std::vector<std::byte> bytes;
    encode_message(message, bytes);
    EXPECT_EQ(decode_message(bytes), message);
  }
}

TEST(RaftCodec, RejectsEveryTruncationAndTrailingBytes) {
  for (const Message& message : samples()) {
    std::vector<std::byte> bytes;
    encode_message(message, bytes);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
      EXPECT_FALSE(decode_message(std::span(bytes).first(size)).has_value()) << size;
    }
    bytes.push_back(std::byte{0});
    EXPECT_FALSE(decode_message(bytes).has_value());
  }
}

TEST(RaftCodec, RejectsUnknownKindsFlagsAndEntryTypes) {
  std::vector<std::byte> bytes;
  encode_message(samples()[1], bytes);
  std::vector<std::byte> kind = bytes;
  kind[16] = std::byte{9};
  EXPECT_FALSE(decode_message(kind).has_value());
  std::vector<std::byte> flag = bytes;
  flag[17] = std::byte{2};
  EXPECT_FALSE(decode_message(flag).has_value());

  bytes.clear();
  encode_message(samples()[2], bytes);
  bytes[17 + 28 + 16] = std::byte{7};
  EXPECT_FALSE(decode_message(bytes).has_value());
}

TEST(RaftCodec, RejectsAnEntryCountLargerThanThePayload) {
  std::vector<std::byte> bytes;
  encode_message(samples()[2], bytes);
  bytes[17 + 24] = std::byte{0xFF};
  bytes[17 + 25] = std::byte{0xFF};
  EXPECT_FALSE(decode_message(bytes).has_value());
}

}  // namespace
}  // namespace rme::io
