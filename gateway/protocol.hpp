#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "codec/client_record.hpp"
#include "engine/event.hpp"
#include "engine/replica.hpp"

namespace rme::gateway {

// Client frames share the peer frame header; the type field tells them apart.
enum class FrameType : std::uint8_t { kSubmit = 16, kAck = 17, kRedirect = 18 };

// Sent once the command's entry is committed and applied on the leader, with its events.
struct Ack {
  std::uint64_t index = 0;
  std::uint64_t client = 0;
  std::uint64_t seq = 0;
  engine::SessionResult result = engine::SessionResult::kUnknownSession;
  std::vector<engine::Event> events;

  friend bool operator==(const Ack&, const Ack&) = default;
};

// Sent by a node that is not the leader instead of submitting; 0 when it knows no leader.
struct Redirect {
  std::uint32_t leader = 0;

  friend bool operator==(const Redirect&, const Redirect&) = default;
};

void encode_redirect(const Redirect& redirect, std::vector<std::byte>& out);
[[nodiscard]] std::optional<Redirect> decode_redirect(std::span<const std::byte> body);

void encode_submit(const codec::ClientRecordBytes& record, std::vector<std::byte>& out);
void encode_ack(const Ack& ack, std::vector<std::byte>& out);

[[nodiscard]] std::optional<engine::ClientCommand> decode_submit(std::span<const std::byte> body);
[[nodiscard]] std::optional<Ack> decode_ack(std::span<const std::byte> body);

}  // namespace rme::gateway
