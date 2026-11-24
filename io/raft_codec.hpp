#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "consensus/types.hpp"

namespace rme::io {

inline constexpr std::uint16_t kRaftFrame = 1;

// Appends the message in a little-endian layout to out.
void encode_message(const consensus::Message& message, std::vector<std::byte>& out);

// Rejects anything that is not exactly one well-formed message, since the bytes come off the
// network.
[[nodiscard]] std::optional<consensus::Message> decode_message(std::span<const std::byte> bytes);

}  // namespace rme::io
