#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/assert.hpp"
#include "io/frame.hpp"
#include "io/raft_codec.hpp"

namespace {

// Whatever decodes must encode back to the same bytes, so no two encodings mean one message.
void check_message(std::span<const std::byte> body) {
  if (const std::optional<rme::consensus::Message> message = rme::io::decode_message(body)) {
    std::vector<std::byte> again;
    rme::io::encode_message(*message, again);
    RME_ASSERT(std::ranges::equal(again, body));
  }
}

// Feeds the stream in pieces whose sizes come from the input, as reads off a socket would.
void check_stream(std::span<const std::byte> stream, std::size_t piece) {
  rme::io::FrameDecoder decoder(64);
  while (!stream.empty()) {
    const std::size_t size = std::min(stream.size(), piece);
    const std::span<std::byte> space = decoder.writable(size);
    std::ranges::copy(stream.first(size), space.begin());
    decoder.commit(size);
    stream = stream.subspan(size);
    while (const std::optional<rme::io::Frame> frame = decoder.next()) {
      RME_ASSERT(frame->body.size() <= rme::io::kMaxFrameBody);
      check_message(frame->body);
    }
    if (decoder.error() != rme::io::FrameError::kNone) {
      return;
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }
  const auto bytes = std::as_bytes(std::span(data, size));
  const auto piece = static_cast<std::size_t>(data[0] % 61U) + 1;
  check_stream(bytes.subspan(1), piece);
  std::vector<std::byte> framed;
  rme::io::encode_frame(rme::io::kRaftFrame, bytes.subspan(1), framed);
  check_stream(framed, piece);
  return 0;
}
