#include "io/frame.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace rme::io {
namespace {

std::vector<std::byte> bytes(std::size_t size, std::uint8_t seed) {
  std::vector<std::byte> out(size);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = static_cast<std::byte>(seed + i * 31);
  }
  return out;
}

void feed(FrameDecoder& decoder, std::span<const std::byte> data) {
  const std::span<std::byte> space = decoder.writable(data.size());
  std::ranges::copy(data, space.begin());
  decoder.commit(data.size());
}

TEST(Frame, PadsBodiesToEightBytes) {
  std::vector<std::byte> out;
  encode_frame(3, bytes(5, 1), out);
  EXPECT_EQ(out.size(), sizeof(FrameHeader) + 8);
  EXPECT_TRUE(std::all_of(out.end() - 3, out.end(), [](std::byte b) { return b == std::byte{0}; }));
}

TEST(Frame, DecodesManyFramesFromOneRead) {
  std::vector<std::byte> stream;
  for (std::uint8_t i = 0; i < 10; ++i) {
    encode_frame(i, bytes(std::size_t{i} * 7, i), stream);
  }
  FrameDecoder decoder(64);
  feed(decoder, stream);
  for (std::uint8_t i = 0; i < 10; ++i) {
    const Frame frame = decoder.next().value_or(Frame{.type = 0xFFFF, .body = {}});
    ASSERT_EQ(frame.type, i);
    EXPECT_TRUE(std::ranges::equal(frame.body, bytes(std::size_t{i} * 7, i)));
  }
  EXPECT_FALSE(decoder.next().has_value());
  EXPECT_EQ(decoder.buffered(), 0U);
  EXPECT_EQ(decoder.error(), FrameError::kNone);
}

TEST(Frame, ReassemblesFramesSplitAtEveryByte) {
  std::vector<std::byte> stream;
  encode_frame(1, bytes(13, 9), stream);
  encode_frame(2, {}, stream);
  encode_frame(3, bytes(300, 4), stream);
  FrameDecoder decoder(16);
  std::vector<std::uint16_t> types;
  for (const std::byte b : stream) {
    feed(decoder, std::span(&b, 1));
    while (const std::optional<Frame> frame = decoder.next()) {
      types.push_back(frame->type);
      if (frame->type == 3) {
        EXPECT_TRUE(std::ranges::equal(frame->body, bytes(300, 4)));
      }
    }
  }
  EXPECT_EQ(types, (std::vector<std::uint16_t>{1, 2, 3}));
}

TEST(Frame, FlippedBitFailsCrcAndStopsDecoding) {
  std::vector<std::byte> stream;
  encode_frame(1, bytes(40, 2), stream);
  encode_frame(1, bytes(40, 3), stream);
  stream[sizeof(FrameHeader) + 17] ^= std::byte{0x10};
  FrameDecoder decoder;
  feed(decoder, stream);
  EXPECT_FALSE(decoder.next().has_value());
  EXPECT_EQ(decoder.error(), FrameError::kBadCrc);
  EXPECT_FALSE(decoder.next().has_value());
}

TEST(Frame, CorruptLengthIsCaughtByCrc) {
  std::vector<std::byte> stream;
  encode_frame(1, bytes(64, 2), stream);
  encode_frame(1, bytes(64, 3), stream);
  FrameHeader header;
  std::memcpy(&header, stream.data(), sizeof header);
  header.length = 8;
  std::memcpy(stream.data(), &header, sizeof header);
  FrameDecoder decoder;
  feed(decoder, stream);
  EXPECT_FALSE(decoder.next().has_value());
  EXPECT_EQ(decoder.error(), FrameError::kBadCrc);
}

TEST(Frame, RejectsUnknownVersionAndOversizedLength) {
  FrameHeader header{.length = 0, .version = 2, .type = 1, .crc = 0, .reserved = 0};
  FrameDecoder old;
  feed(old, std::as_bytes(std::span(&header, 1)));
  EXPECT_FALSE(old.next().has_value());
  EXPECT_EQ(old.error(), FrameError::kBadVersion);

  header = {
      .length = kMaxFrameBody + 1, .version = kFrameVersion, .type = 1, .crc = 0, .reserved = 0};
  FrameDecoder big;
  feed(big, std::as_bytes(std::span(&header, 1)));
  EXPECT_FALSE(big.next().has_value());
  EXPECT_EQ(big.error(), FrameError::kTooLarge);
}

TEST(Frame, GrowsForAFrameLargerThanTheBuffer) {
  std::vector<std::byte> stream;
  encode_frame(5, bytes(10'000, 6), stream);
  FrameDecoder decoder(256);
  std::size_t at = 0;
  while (at < stream.size()) {
    const std::size_t chunk = std::min<std::size_t>(1000, stream.size() - at);
    feed(decoder, std::span(stream).subspan(at, chunk));
    at += chunk;
  }
  const Frame frame = decoder.next().value_or(Frame{.type = 0xFFFF, .body = {}});
  ASSERT_EQ(frame.type, 5);
  EXPECT_EQ(frame.body.size(), 10'000U);
}

}  // namespace
}  // namespace rme::io
