#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace rme::io {

static_assert(std::endian::native == std::endian::little);

struct FrameHeader {
  std::uint32_t length = 0;
  std::uint16_t version = 0;
  std::uint16_t type = 0;
  std::uint32_t crc = 0;
  std::uint32_t reserved = 0;
};

static_assert(sizeof(FrameHeader) == 16);
static_assert(std::is_trivially_copyable_v<FrameHeader>);
static_assert(std::has_unique_object_representations_v<FrameHeader>);

inline constexpr std::uint16_t kFrameVersion = 1;
inline constexpr std::size_t kMaxFrameBody = std::size_t{64} << 20U;

[[nodiscard]] constexpr std::size_t padded(std::size_t size) noexcept {
  return (size + 7U) & ~std::size_t{7};
}

// Appends a header, the body and zero padding to a multiple of 8 bytes.
void encode_frame(std::uint16_t type, std::span<const std::byte> body, std::vector<std::byte>& out);

struct Frame {
  std::uint16_t type = 0;
  std::span<const std::byte> body;
};

enum class FrameError : std::uint8_t { kNone, kBadVersion, kTooLarge, kBadCrc };

// Reassembles frames from a byte stream that arrives in arbitrary pieces. Reads go straight into
// the decoder's buffer, so one read can carry many frames and none is copied before decode.
class FrameDecoder {
 public:
  explicit FrameDecoder(std::size_t capacity = std::size_t{1} << 20U);

  // Space for the next read; at least min_space bytes, compacting or growing as needed.
  [[nodiscard]] std::span<std::byte> writable(std::size_t min_space = 4096);
  void commit(std::size_t bytes) noexcept;

  // The next whole frame, valid until the next writable(). Stops for good on a bad frame.
  [[nodiscard]] std::optional<Frame> next() noexcept;

  [[nodiscard]] FrameError error() const noexcept { return error_; }
  [[nodiscard]] std::size_t buffered() const noexcept { return end_ - begin_; }

 private:
  std::vector<std::byte> buffer_;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
  FrameError error_ = FrameError::kNone;
};

}  // namespace rme::io
