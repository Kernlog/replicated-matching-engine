#include "gateway/protocol.hpp"

#include <array>
#include <cstring>

#include "engine/event_words.hpp"
#include "io/frame.hpp"

namespace rme::gateway {

namespace {

struct AckHeader {
  std::uint64_t index = 0;
  std::uint64_t client = 0;
  std::uint64_t seq = 0;
  std::uint32_t result = 0;
  std::uint32_t events = 0;
};

static_assert(std::has_unique_object_representations_v<AckHeader>);

constexpr auto kResults = static_cast<std::uint32_t>(engine::SessionResult::kOutOfOrder) + 1;

template <typename T>
void append(std::vector<std::byte>& out, const T& value) {
  const std::size_t at = out.size();
  out.resize(at + sizeof value);
  std::memcpy(&out[at], &value, sizeof value);
}

}  // namespace

void encode_redirect(const Redirect& redirect, std::vector<std::byte>& out) {
  const std::array<std::uint32_t, 2> words{redirect.leader, 0};
  io::encode_frame(static_cast<std::uint16_t>(FrameType::kRedirect),
                   std::as_bytes(std::span(words)), out);
}

std::optional<Redirect> decode_redirect(std::span<const std::byte> body) {
  std::array<std::uint32_t, 2> words{};
  if (body.size() != sizeof words) {
    return std::nullopt;
  }
  std::memcpy(words.data(), body.data(), sizeof words);
  if (words[1] != 0) {
    return std::nullopt;
  }
  return Redirect{.leader = words[0]};
}

void encode_submit(const codec::ClientRecordBytes& record, std::vector<std::byte>& out) {
  io::encode_frame(static_cast<std::uint16_t>(FrameType::kSubmit), record, out);
}

void encode_ack(const Ack& ack, std::vector<std::byte>& out) {
  std::vector<std::byte> body;
  body.reserve(sizeof(AckHeader) + ack.events.size() * sizeof(engine::EventWords));
  append(body, AckHeader{.index = ack.index,
                         .client = ack.client,
                         .seq = ack.seq,
                         .result = static_cast<std::uint32_t>(ack.result),
                         .events = static_cast<std::uint32_t>(ack.events.size())});
  for (const engine::Event& event : ack.events) {
    append(body, engine::to_words(event));
  }
  io::encode_frame(static_cast<std::uint16_t>(FrameType::kAck), body, out);
}

std::optional<engine::ClientCommand> decode_submit(std::span<const std::byte> body) {
  return codec::decode_client(body);
}

std::optional<Ack> decode_ack(std::span<const std::byte> body) {
  AckHeader header;
  if (body.size() < sizeof header) {
    return std::nullopt;
  }
  std::memcpy(&header, body.data(), sizeof header);
  const std::span<const std::byte> rest = body.subspan(sizeof header);
  if (header.result >= kResults ||
      rest.size() != std::size_t{header.events} * sizeof(engine::EventWords)) {
    return std::nullopt;
  }
  Ack ack{.index = header.index,
          .client = header.client,
          .seq = header.seq,
          .result = static_cast<engine::SessionResult>(header.result),
          .events = {}};
  ack.events.reserve(header.events);
  for (std::size_t i = 0; i < header.events; ++i) {
    engine::EventWords words{};
    std::memcpy(words.data(), &rest[i * sizeof words], sizeof words);
    std::optional<engine::Event> event = engine::from_words(words);
    if (!event) {
      return std::nullopt;
    }
    ack.events.push_back(*event);
  }
  return ack;
}

}  // namespace rme::gateway
