#include "log/command_log.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include "codec/record.hpp"

namespace rme::log {
namespace {

constexpr std::size_t kReadBytes = std::size_t{1} << 20U;
constexpr std::size_t kWriteBytes = std::size_t{64} << 10U;

[[noreturn]] void fail(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

std::uint64_t valid_prefix(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return 0;
  }
  LogReader reader(path);
  while (reader.next()) {
  }
  return reader.valid_bytes();
}

}  // namespace

File::File(const std::filesystem::path& path, int flags)
    : fd_(::open(path.c_str(), flags | O_CLOEXEC, 0644)) {  // NOLINT(*-vararg)
  if (fd_ < 0) {
    fail("open " + path.string());
  }
}

File::File(File&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

File::~File() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

LogReader::LogReader(const std::filesystem::path& path)
    : file_(path, O_RDONLY), buffer_(kReadBytes) {
  ::posix_fadvise(file_.fd(), 0, 0, POSIX_FADV_SEQUENTIAL);
}

bool LogReader::refill() {
  std::copy(buffer_.begin() + static_cast<std::ptrdiff_t>(begin_),
            buffer_.begin() + static_cast<std::ptrdiff_t>(end_), buffer_.begin());
  end_ -= begin_;
  begin_ = 0;
  while (end_ < codec::kRecordSize) {
    const ssize_t n = ::read(file_.fd(), &buffer_[end_], buffer_.size() - end_);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      fail("read");
    }
    if (n == 0) {
      return false;
    }
    end_ += static_cast<std::size_t>(n);
  }
  return true;
}

std::optional<engine::Command> LogReader::next() {
  if (done_ || (end_ - begin_ < codec::kRecordSize && !refill())) {
    done_ = true;
    return std::nullopt;
  }
  auto command = codec::decode(std::span(buffer_).subspan(begin_, codec::kRecordSize));
  if (!command) {
    done_ = true;
    return std::nullopt;
  }
  begin_ += codec::kRecordSize;
  valid_bytes_ += codec::kRecordSize;
  return command;
}

LogWriter::LogWriter(const std::filesystem::path& path)
    : file_(path, O_WRONLY | O_CREAT), offset_(valid_prefix(path)) {
  if (::ftruncate(file_.fd(), static_cast<off_t>(offset_)) != 0) {
    fail("ftruncate " + path.string());
  }
  buffer_.reserve(kWriteBytes);
}

void LogWriter::append(const engine::Command& command) {
  const codec::RecordBytes record = codec::encode(command);
  buffer_.insert(buffer_.end(), record.begin(), record.end());
  if (buffer_.size() >= kWriteBytes) {
    flush();
  }
}

void LogWriter::flush() {
  std::size_t written = 0;
  while (written < buffer_.size()) {
    const ssize_t n = ::pwrite(file_.fd(), &buffer_[written], buffer_.size() - written,
                               static_cast<off_t>(offset_ + written));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      fail("pwrite");
    }
    written += static_cast<std::size_t>(n);
  }
  offset_ += written;
  buffer_.clear();
}

void LogWriter::sync() {
  flush();
  if (::fdatasync(file_.fd()) != 0) {
    fail("fdatasync");
  }
}

}  // namespace rme::log
