#pragma once

#include <cstddef>
#include <span>

#include "codec/client_record.hpp"
#include "codec/record.hpp"

namespace rme::codec {

namespace detail {

template <std::size_t Size, typename Decode, typename F>
bool for_each_record(std::span<const std::byte> batch, Decode decode_one, F& fn) {
  if (batch.size() % Size != 0) {
    return false;
  }
  for (std::size_t offset = 0; offset < batch.size(); offset += Size) {
    if (!decode_one(batch.subspan(offset, Size))) {
      return false;
    }
  }
  for (std::size_t offset = 0; offset < batch.size(); offset += Size) {
    if (const auto decoded = decode_one(batch.subspan(offset, Size))) {
      fn(*decoded);
    }
  }
  return true;
}

}  // namespace detail

// Visits every command in a batch of back-to-back records only if the whole batch decodes.
template <typename F>
[[nodiscard]] bool for_each_command(std::span<const std::byte> batch, F fn) {
  return detail::for_each_record<kRecordSize>(
      batch, [](std::span<const std::byte> bytes) { return decode(bytes); }, fn);
}

// Visits every client command in a batch of back-to-back client records only if all decode.
template <typename F>
[[nodiscard]] bool for_each_client_command(std::span<const std::byte> batch, F fn) {
  return detail::for_each_record<kClientRecordSize>(
      batch, [](std::span<const std::byte> bytes) { return decode_client(bytes); }, fn);
}

}  // namespace rme::codec
