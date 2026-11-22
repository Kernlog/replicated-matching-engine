#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

#include "codec/crc32c.hpp"
#include "core/types.hpp"
#include "engine/command.hpp"

namespace rme::codec {

static_assert(std::endian::native == std::endian::little, "records are stored little-endian");

inline constexpr std::uint16_t kRecordVersion = 1;

enum class RecordType : std::uint8_t { kLimit = 1, kMarket = 2, kCancel = 3, kReplace = 4 };

struct CommandRecord {
  std::uint16_t version = kRecordVersion;
  RecordType type = RecordType::kCancel;
  std::uint8_t side = 0;
  std::uint32_t crc = 0;
  std::uint64_t id = 0;
  std::int64_t price = 0;
  std::int64_t qty = 0;
};

inline constexpr std::size_t kRecordSize = sizeof(CommandRecord);
using RecordBytes = std::array<std::byte, kRecordSize>;

static_assert(kRecordSize == 32 && alignof(CommandRecord) == 8);
static_assert(offsetof(CommandRecord, crc) == 4 && offsetof(CommandRecord, id) == 8 &&
              offsetof(CommandRecord, price) == 16 && offsetof(CommandRecord, qty) == 24);
static_assert(std::has_unique_object_representations_v<CommandRecord>);

namespace detail {

constexpr std::uint8_t side_byte(Side side) noexcept {
  return side == Side::kBuy ? 1 : 2;
}

constexpr std::optional<Side> side_of(std::uint8_t byte) noexcept {
  if (byte == 1) {
    return Side::kBuy;
  }
  if (byte == 2) {
    return Side::kSell;
  }
  return std::nullopt;
}

constexpr CommandRecord to_record(const engine::LimitOrder& c) noexcept {
  return {.type = RecordType::kLimit,
          .side = side_byte(c.side),
          .id = c.id.value(),
          .price = c.price.value(),
          .qty = c.qty.value()};
}

constexpr CommandRecord to_record(const engine::MarketOrder& c) noexcept {
  return {.type = RecordType::kMarket,
          .side = side_byte(c.side),
          .id = c.id.value(),
          .qty = c.qty.value()};
}

constexpr CommandRecord to_record(const engine::Cancel& c) noexcept {
  return {.type = RecordType::kCancel, .id = c.id.value()};
}

constexpr CommandRecord to_record(const engine::Replace& c) noexcept {
  return {.type = RecordType::kReplace,
          .id = c.id.value(),
          .price = c.price.value(),
          .qty = c.qty.value()};
}

constexpr std::uint32_t checksum(CommandRecord record) noexcept {
  record.crc = 0;
  return crc32c(std::bit_cast<RecordBytes>(record));
}

}  // namespace detail

[[nodiscard]] constexpr RecordBytes encode(const engine::Command& command) {
  CommandRecord record = std::visit([](const auto& c) { return detail::to_record(c); }, command);
  record.crc = detail::checksum(record);
  return std::bit_cast<RecordBytes>(record);
}

[[nodiscard]] constexpr std::optional<engine::Command> decode(
    std::span<const std::byte> bytes) noexcept {
  if (bytes.size() != kRecordSize) {
    return std::nullopt;
  }
  RecordBytes raw{};
  std::ranges::copy(bytes, raw.begin());
  const auto r = std::bit_cast<CommandRecord>(raw);
  if (r.version != kRecordVersion || r.crc != detail::checksum(r)) {
    return std::nullopt;
  }
  const OrderId id(r.id);
  const std::optional<Side> side = detail::side_of(r.side);
  switch (r.type) {
    case RecordType::kLimit:
      if (side) {
        return engine::LimitOrder{id, *side, Price(r.price), Qty(r.qty)};
      }
      break;
    case RecordType::kMarket:
      if (side && r.price == 0) {
        return engine::MarketOrder{id, *side, Qty(r.qty)};
      }
      break;
    case RecordType::kCancel:
      if (r.side == 0 && r.price == 0 && r.qty == 0) {
        return engine::Cancel{id};
      }
      break;
    case RecordType::kReplace:
      if (r.side == 0) {
        return engine::Replace{id, Price(r.price), Qty(r.qty)};
      }
      break;
  }
  return std::nullopt;
}

}  // namespace rme::codec
