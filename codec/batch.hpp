#pragma once

#include <cstddef>
#include <span>

#include "codec/record.hpp"

namespace rme::codec {

// Visits every command in a batch of back-to-back records only if the whole batch decodes.
template <typename F>
[[nodiscard]] bool for_each_command(std::span<const std::byte> batch, F fn) {
  if (batch.size() % kRecordSize != 0) {
    return false;
  }
  for (std::size_t offset = 0; offset < batch.size(); offset += kRecordSize) {
    if (!decode(batch.subspan(offset, kRecordSize))) {
      return false;
    }
  }
  for (std::size_t offset = 0; offset < batch.size(); offset += kRecordSize) {
    if (const auto command = decode(batch.subspan(offset, kRecordSize))) {
      fn(*command);
    }
  }
  return true;
}

}  // namespace rme::codec
