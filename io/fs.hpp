#pragma once

#include <sys/types.h>
#include <sys/uio.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace rme::io {

[[noreturn]] void throw_errno(const std::string& what);

// Data durability for an fd: fdatasync on Linux, F_FULLFSYNC on Darwin, where fsync only reaches
// the drive cache.
void sync_data(int fd);
// Same, plus metadata such as the file size.
void sync_file(int fd);
void sync_dir(const std::filesystem::path& dir);

// Reserves blocks so later writes never change the file size and fdatasync skips the inode.
void preallocate(int fd, std::uint64_t bytes);

// Writes every byte the iovecs describe at offset, retrying short writes. Consumes iov.
void pwritev_all(int fd, std::span<iovec> iov, off_t offset);

}  // namespace rme::io
