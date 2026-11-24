#include "io/frame.hpp"

#include <algorithm>
#include <cstring>

#include "codec/crc32c.hpp"
#include "core/assert.hpp"

namespace rme::io {

namespace {

std::uint32_t frame_crc(FrameHeader header, std::span<const std::byte> body) noexcept {
  header.crc = 0;
  const std::uint32_t seed = codec::crc32c(std::as_bytes(std::span(&header, 1)));
  return codec::crc32c(body, seed);
}

}  // namespace

void encode_frame(std::uint16_t type, std::span<const std::byte> body,
                  std::vector<std::byte>& out) {
  RME_ASSERT(body.size() <= kMaxFrameBody);
  FrameHeader header{.length = static_cast<std::uint32_t>(body.size()),
                     .version = kFrameVersion,
                     .type = type,
                     .crc = 0,
                     .reserved = 0};
  header.crc = frame_crc(header, body);
  const std::size_t at = out.size();
  out.resize(at + sizeof header + padded(body.size()));
  std::memcpy(&out[at], &header, sizeof header);
  if (!body.empty()) {
    std::memcpy(&out[at + sizeof header], body.data(), body.size());
  }
}

FrameDecoder::FrameDecoder(std::size_t capacity) : buffer_(capacity) {}

std::span<std::byte> FrameDecoder::writable(std::size_t min_space) {
  if (buffer_.size() - end_ < min_space) {
    std::copy(buffer_.begin() + static_cast<std::ptrdiff_t>(begin_),
              buffer_.begin() + static_cast<std::ptrdiff_t>(end_), buffer_.begin());
    end_ -= begin_;
    begin_ = 0;
    if (buffer_.size() - end_ < min_space) {
      buffer_.resize(std::max(buffer_.size() * 2, end_ + min_space));
    }
  }
  return std::span(buffer_).subspan(end_);
}

void FrameDecoder::commit(std::size_t bytes) noexcept {
  RME_ASSERT(end_ + bytes <= buffer_.size());
  end_ += bytes;
}

std::optional<Frame> FrameDecoder::next() noexcept {
  if (error_ != FrameError::kNone || buffered() < sizeof(FrameHeader)) {
    return std::nullopt;
  }
  FrameHeader header;
  std::memcpy(&header, &buffer_[begin_], sizeof header);
  if (header.version != kFrameVersion) {
    error_ = FrameError::kBadVersion;
    return std::nullopt;
  }
  if (header.length > kMaxFrameBody) {
    error_ = FrameError::kTooLarge;
    return std::nullopt;
  }
  const std::size_t total = sizeof header + padded(header.length);
  if (buffered() < total) {
    return std::nullopt;
  }
  const std::span<const std::byte> body =
      std::span(buffer_).subspan(begin_ + sizeof header, header.length);
  if (frame_crc(header, body) != header.crc) {
    error_ = FrameError::kBadCrc;
    return std::nullopt;
  }
  begin_ += total;
  return Frame{.type = header.type, .body = body};
}

}  // namespace rme::io
