#include "io/atomic_file.hpp"

#include <fcntl.h>
#include <sys/uio.h>

#include <array>

#include "io/fs.hpp"
#include "log/command_log.hpp"

namespace rme::io {

void write_atomic(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    const log::File file(temp, O_WRONLY | O_CREAT | O_TRUNC);
    std::array<iovec, 1> iov{{{const_cast<std::byte*>(bytes.data()),  // NOLINT(*-const-cast)
                               bytes.size()}}};
    if (!bytes.empty()) {
      pwritev_all(file.fd(), iov, 0);
    }
    sync_file(file.fd());
  }
  std::filesystem::rename(temp, path);
  sync_dir(path.parent_path());
}

}  // namespace rme::io
