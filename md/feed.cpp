#include "md/feed.hpp"

#include <algorithm>
#include <cstring>

#include "codec/crc32c.hpp"
#include "core/assert.hpp"

namespace rme::md {

namespace {

struct Wire {
  std::uint64_t index = 0;
  std::uint32_t position = 0;
  std::uint32_t count = 0;
  codec::ClientRecordBytes record{};
};

static_assert(sizeof(Wire) == kMessageBytes);

std::uint32_t packet_crc(PacketHeader header, std::span<const std::byte> body) noexcept {
  header.crc = 0;
  return codec::crc32c(body, codec::crc32c(std::as_bytes(std::span(&header, 1))));
}

}  // namespace

void encode_packet(std::span<const Message> messages, std::vector<std::byte>& out) {
  RME_ASSERT(!messages.empty() && messages.size() <= kMessagesPerPacket);
  const std::size_t at = out.size();
  out.resize(at + sizeof(PacketHeader) + messages.size() * kMessageBytes);
  for (std::size_t i = 0; i < messages.size(); ++i) {
    RME_ASSERT(messages[i].seq == messages.front().seq + i);
    const Wire wire{.index = messages[i].index,
                    .position = messages[i].position,
                    .count = messages[i].count,
                    .record = messages[i].record};
    std::memcpy(&out[at + sizeof(PacketHeader) + i * kMessageBytes], &wire, sizeof wire);
  }
  PacketHeader header{.first_seq = messages.front().seq,
                      .messages = static_cast<std::uint32_t>(messages.size()),
                      .crc = 0};
  header.crc = packet_crc(header, std::span(out).subspan(at + sizeof header));
  std::memcpy(&out[at], &header, sizeof header);
}

std::optional<std::vector<Message>> decode_packet(std::span<const std::byte> bytes) {
  PacketHeader header;
  if (bytes.size() < sizeof header) {
    return std::nullopt;
  }
  std::memcpy(&header, bytes.data(), sizeof header);
  const std::span<const std::byte> body = bytes.subspan(sizeof header);
  if (header.messages == 0 || header.messages > kMessagesPerPacket ||
      body.size() != std::size_t{header.messages} * kMessageBytes ||
      packet_crc(header, body) != header.crc) {
    return std::nullopt;
  }
  std::vector<Message> out(header.messages);
  for (std::size_t i = 0; i < out.size(); ++i) {
    Wire wire;
    std::memcpy(&wire, &body[i * kMessageBytes], sizeof wire);
    out[i] = Message{.seq = header.first_seq + i,
                     .index = wire.index,
                     .position = wire.position,
                     .count = wire.count,
                     .record = wire.record};
  }
  return out;
}

void split_entry(std::uint64_t seq, std::uint64_t index, std::span<const std::byte> data,
                 std::vector<Message>& out) {
  const std::size_t count = data.size() / codec::kClientRecordSize;
  if (count == 0) {
    out.push_back(Message{.seq = seq, .index = index, .position = 0, .count = 0, .record = {}});
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    Message message{.seq = seq + i,
                    .index = index,
                    .position = static_cast<std::uint32_t>(i),
                    .count = static_cast<std::uint32_t>(count),
                    .record = {}};
    std::copy_n(data.subspan(i * codec::kClientRecordSize).begin(), codec::kClientRecordSize,
                message.record.begin());
    out.push_back(message);
  }
}

}  // namespace rme::md
