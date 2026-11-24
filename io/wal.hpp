#pragma once

#include <sys/uio.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "consensus/types.hpp"
#include "log/command_log.hpp"

namespace rme::io {

enum class RecordKind : std::uint8_t { kEntry = 1, kSnapshot = 2 };

struct RecordHeader {
  std::uint32_t length = 0;
  std::uint32_t crc = 0;
  std::uint64_t term = 0;
  std::uint64_t index = 0;
  RecordKind kind = RecordKind::kEntry;
  consensus::EntryType entry_type = consensus::EntryType::kNoop;
  std::array<std::uint8_t, 6> reserved{};
};

static_assert(sizeof(RecordHeader) == 32);
static_assert(std::has_unique_object_representations_v<RecordHeader>);

struct WalOptions {
  std::uint64_t segment_bytes = std::uint64_t{64} << 20U;
};

struct WalStats {
  std::uint64_t writes = 0;
  std::uint64_t syncs = 0;
  std::uint64_t bytes = 0;
};

// Write-ahead log of Raft entries in preallocated segment files. A record whose index is at or
// below the last one replaces that suffix, so truncation is an append. A snapshot mark drops the
// prefix it covers, and the rest too unless the log holds the mark's index at the mark's term.
class Wal {
 public:
  Wal(std::filesystem::path dir, WalOptions options = {});

  // The entries after the last snapshot mark, as the log held them when opened.
  [[nodiscard]] std::vector<consensus::Entry> take_entries() noexcept {
    return std::move(entries_);
  }
  [[nodiscard]] consensus::Index mark_index() const noexcept { return mark_index_; }
  [[nodiscard]] consensus::Term mark_term() const noexcept { return mark_term_; }

  void append(std::span<const consensus::Entry> entries);
  void mark_snapshot(consensus::Index index, consensus::Term term);
  // Makes every earlier append and mark durable.
  void sync();

  [[nodiscard]] const WalStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }

 private:
  void replay();
  std::uint64_t replay_segment(const std::filesystem::path& path);
  void apply(const RecordHeader& header, std::span<const std::byte> body);
  void stage(const RecordHeader& header, std::span<const std::byte> body);
  void flush();
  void roll();

  std::filesystem::path dir_;
  WalOptions options_;
  std::optional<log::File> current_;
  std::uint64_t sequence_ = 0;
  std::vector<log::File> unsynced_;
  std::uint64_t offset_ = 0;
  std::uint64_t staged_bytes_ = 0;
  std::vector<RecordHeader> headers_;
  std::vector<iovec> iov_;
  std::vector<consensus::Entry> entries_;
  consensus::Index mark_index_;
  consensus::Term mark_term_;
  WalStats stats_;
};

[[nodiscard]] std::filesystem::path segment_path(const std::filesystem::path& dir,
                                                 std::uint64_t sequence);

}  // namespace rme::io
