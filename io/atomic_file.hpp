#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

namespace rme::io {

// Replaces path so a crash leaves either the old bytes or the new ones: write a temp file, fsync
// it, rename it over path, then fsync the directory so the rename itself is durable.
void write_atomic(const std::filesystem::path& path, std::span<const std::byte> bytes);

}  // namespace rme::io
