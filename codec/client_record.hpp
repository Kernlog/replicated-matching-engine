#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

#include "codec/crc32c.hpp"
#include "codec/record.hpp"
#include "engine/client_command.hpp"

namespace rme::codec {

struct ClientRecord {
  std::uint64_t client = 0;
  std::uint64_t seq = 0;
  CommandRecord command;
};

inline constexpr std::size_t kClientRecordSize = sizeof(ClientRecord);
using ClientRecordBytes = std::array<std::byte, kClientRecordSize>;

static_assert(kClientRecordSize == 48 && offsetof(ClientRecord, command) == 16);
static_assert(std::has_unique_object_representations_v<ClientRecord>);

namespace detail {

constexpr std::uint32_t session_seed(std::uint64_t client, std::uint64_t seq) noexcept {
  return crc32c(std::bit_cast<std::array<std::byte, 16>>(std::array{client, seq}));
}

}  // namespace detail

[[nodiscard]] constexpr ClientRecordBytes encode(const engine::ClientCommand& command) {
  ClientRecord record{
      .client = command.client, .seq = command.seq, .command = detail::to_record(command.command)};
  record.command.crc =
      detail::checksum(record.command, detail::session_seed(record.client, record.seq));
  return std::bit_cast<ClientRecordBytes>(record);
}

[[nodiscard]] constexpr ClientRecordBytes encode_register() {
  return encode(engine::ClientCommand{});
}

[[nodiscard]] constexpr std::optional<engine::ClientCommand> decode_client(
    std::span<const std::byte> bytes) {
  if (bytes.size() != kClientRecordSize) {
    return std::nullopt;
  }
  const auto r = std::bit_cast<ClientRecord>(detail::copied<kClientRecordSize>(bytes));
  if (r.command.crc != detail::checksum(r.command, detail::session_seed(r.client, r.seq))) {
    return std::nullopt;
  }
  const std::optional<engine::Command> command = detail::to_command(r.command);
  if (!command) {
    return std::nullopt;
  }
  engine::ClientCommand decoded{.client = r.client, .seq = r.seq, .command = *command};
  if (r.client == engine::kRegisterClient && decoded != engine::ClientCommand{}) {
    return std::nullopt;
  }
  return decoded;
}

}  // namespace rme::codec
