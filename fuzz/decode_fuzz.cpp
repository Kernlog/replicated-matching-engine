#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "codec/record.hpp"
#include "core/assert.hpp"

namespace {

void check_canonical(std::span<const std::byte> bytes) {
  if (const auto command = rme::codec::decode(bytes)) {
    const rme::codec::RecordBytes again = rme::codec::encode(*command);
    RME_ASSERT(std::ranges::equal(again, bytes));
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto bytes = std::as_bytes(std::span(data, size));
  check_canonical(bytes);
  if (size == rme::codec::kRecordSize) {
    rme::codec::RecordBytes raw{};
    std::ranges::copy(bytes, raw.begin());
    auto record = std::bit_cast<rme::codec::CommandRecord>(raw);
    record.crc = rme::codec::detail::checksum(record);
    check_canonical(std::bit_cast<rme::codec::RecordBytes>(record));
  }
  return 0;
}
