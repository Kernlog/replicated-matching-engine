#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "codec/client_record.hpp"

namespace rme::md {

// The feed carries the sequenced command stream itself: one message per client record of each
// committed entry (an empty entry still gets one message), numbered without gaps. A subscriber
// that applies them in order runs the same replica as the cluster and ends on the same book.
struct Message {
  std::uint64_t seq = 0;
  std::uint64_t index = 0;
  std::uint32_t position = 0;
  std::uint32_t count = 0;
  codec::ClientRecordBytes record{};
};

struct PacketHeader {
  std::uint64_t first_seq = 0;
  std::uint32_t messages = 0;
  std::uint32_t crc = 0;
};

inline constexpr std::size_t kMessageBytes = 64;
// Under a 1500-byte Ethernet MTU after IP and UDP headers, so datagrams never fragment.
inline constexpr std::size_t kMaxDatagram = 1400;
inline constexpr std::size_t kMessagesPerPacket =
    (kMaxDatagram - sizeof(PacketHeader)) / kMessageBytes;

static_assert(sizeof(PacketHeader) == 16);
static_assert(std::has_unique_object_representations_v<PacketHeader>);
static_assert(std::endian::native == std::endian::little);

// Appends one datagram holding messages, whose seqs must be consecutive.
void encode_packet(std::span<const Message> messages, std::vector<std::byte>& out);
[[nodiscard]] std::optional<std::vector<Message>> decode_packet(std::span<const std::byte> bytes);

// Splits a committed entry into feed messages starting at seq.
void split_entry(std::uint64_t seq, std::uint64_t index, std::span<const std::byte> data,
                 std::vector<Message>& out);

}  // namespace rme::md
