#pragma once

#include <unistd.h>

#include <filesystem>
#include <string>

namespace rme::io {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "rme-XXXXXX").string();
    path_ = ::mkdtemp(pattern.data());
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace rme::io
