#include "io/fs.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <span>
#include <system_error>

#include "log/command_log.hpp"

namespace rme::io {

void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

void sync_data(int fd) {
#if defined(__APPLE__)
  if (::fcntl(fd, F_FULLFSYNC) != 0) {  // NOLINT(*-vararg)
    throw_errno("F_FULLFSYNC");
  }
#else
  if (::fdatasync(fd) != 0) {
    throw_errno("fdatasync");
  }
#endif
}

void sync_file(int fd) {
#if defined(__APPLE__)
  sync_data(fd);
#else
  if (::fsync(fd) != 0) {
    throw_errno("fsync");
  }
#endif
}

void sync_dir(const std::filesystem::path& dir) {
  const log::File file(dir, O_RDONLY | O_DIRECTORY);
  sync_file(file.fd());
}

void preallocate(int fd, std::uint64_t bytes) {
#if defined(__linux__)
  if (const int error = ::posix_fallocate(fd, 0, static_cast<off_t>(bytes)); error != 0) {
    errno = error;
    throw_errno("posix_fallocate");
  }
#else
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    throw_errno("ftruncate");
  }
#endif
}

void pwritev_all(int fd, std::span<iovec> iov, off_t offset) {
  while (!iov.empty()) {
    const std::size_t count = std::min<std::size_t>(iov.size(), IOV_MAX);
    const ssize_t written = ::pwritev(fd, iov.data(), static_cast<int>(count), offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("pwritev");
    }
    if (written == 0) {
      errno = EIO;
      throw_errno("pwritev wrote nothing");
    }
    offset += written;
    auto left = static_cast<std::size_t>(written);
    while (!iov.empty() && left >= iov.front().iov_len) {
      left -= iov.front().iov_len;
      iov = iov.subspan(1);
    }
    if (left != 0) {
      iov.front().iov_base =
          std::span(static_cast<std::byte*>(iov.front().iov_base), iov.front().iov_len)
              .subspan(left)
              .data();
      iov.front().iov_len -= left;
    }
  }
}

}  // namespace rme::io
