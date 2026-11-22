#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "engine/command.hpp"

namespace rme::log {

class File {
 public:
  File(const std::filesystem::path& path, int flags);
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  ~File();

  [[nodiscard]] int fd() const noexcept { return fd_; }

 private:
  int fd_ = -1;
};

class LogReader {
 public:
  explicit LogReader(const std::filesystem::path& path);

  std::optional<engine::Command> next();

  [[nodiscard]] std::uint64_t valid_bytes() const noexcept { return valid_bytes_; }

 private:
  bool refill();

  File file_;
  std::vector<std::byte> buffer_;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
  std::uint64_t valid_bytes_ = 0;
  bool done_ = false;
};

class LogWriter {
 public:
  explicit LogWriter(const std::filesystem::path& path);

  void append(const engine::Command& command);
  // Appends are buffered; flush writes them out and only sync makes them durable.
  void flush();
  void sync();

  [[nodiscard]] std::uint64_t size() const noexcept { return offset_ + buffer_.size(); }

 private:
  File file_;
  std::vector<std::byte> buffer_;
  std::uint64_t offset_ = 0;
};

}  // namespace rme::log
