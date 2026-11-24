#include <fcntl.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "core/rng.hpp"
#include "io/fs.hpp"
#include "io/wal.hpp"
#include "tests/io/temp_dir.hpp"

namespace rme::io {
namespace {

using consensus::Entry;
using consensus::EntryType;
using consensus::Index;
using consensus::Term;

constexpr std::size_t kSector = 512;

std::vector<Entry> run(std::uint64_t term, std::uint64_t from, std::uint64_t to) {
  std::vector<Entry> out;
  for (std::uint64_t i = from; i <= to; ++i) {
    out.push_back(Entry{Term(term), Index(i), EntryType::kCommand,
                        std::vector<std::byte>(i % 61, static_cast<std::byte>(i | 1U))});
  }
  return out;
}

std::vector<Entry> concat(std::vector<Entry> a, const std::vector<Entry>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT(*-reinterpret-cast)
            static_cast<std::streamsize>(bytes.size()));
}

struct Images {
  std::vector<std::byte> synced;
  std::vector<std::byte> full;
  std::uint64_t synced_end = 0;
  std::uint64_t full_end = 0;
};

// The segment after batch a is synced, and after batch b is written in full.
Images images(const std::vector<Entry>& a, const std::vector<Entry>& b, WalOptions options) {
  const TempDir dir;
  Images out;
  Wal wal(dir.path(), options);
  wal.append(a);
  wal.sync();
  out.synced = read_file(segment_path(dir.path(), 1));
  out.synced_end = wal.stats().bytes;
  wal.append(b);
  wal.sync();
  out.full = read_file(segment_path(dir.path(), 1));
  out.full_end = wal.stats().bytes;
  return out;
}

// After recovery the log is the synced batch plus whole records of the unsynced one, and appends
// that follow survive a second reopen without stale bytes coming back.
void expect_recovers(const std::vector<std::byte>& image, const std::vector<Entry>& a,
                     const std::vector<Entry>& b, WalOptions options) {
  const TempDir dir;
  write_bytes(segment_path(dir.path(), 1), image);
  std::size_t kept = 0;
  {
    Wal wal(dir.path(), options);
    const std::vector<Entry> entries = wal.take_entries();
    ASSERT_GE(entries.size(), a.size());
    kept = entries.size() - a.size();
    ASSERT_LE(kept, b.size());
    ASSERT_EQ(entries,
              concat(a, std::vector(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(kept))));
    wal.append(run(9, a.size() + kept + 1, a.size() + kept + 3));
    wal.sync();
  }
  Wal wal(dir.path(), options);
  const std::vector<Entry> prefix(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(kept));
  EXPECT_EQ(wal.take_entries(),
            concat(concat(a, prefix), run(9, a.size() + kept + 1, a.size() + kept + 3)));
}

TEST(WalCrash, CrashAfterEverySectorOfAnUnsyncedBatch) {
  const WalOptions options{.segment_bytes = 1U << 16U};
  const std::vector<Entry> a = run(1, 1, 30);
  const std::vector<Entry> b = run(2, 31, 150);
  const Images image = images(a, b, options);
  const std::uint64_t first = image.synced_end / kSector * kSector;
  for (std::uint64_t cut = first; cut <= image.full_end + kSector; cut += kSector) {
    std::vector<std::byte> crashed = image.synced;
    std::copy_n(image.full.begin() + static_cast<std::ptrdiff_t>(first), cut - first,
                crashed.begin() + static_cast<std::ptrdiff_t>(first));
    SCOPED_TRACE(cut);
    expect_recovers(crashed, a, b, options);
  }
}

TEST(WalCrash, CrashPersistingAnySubsetOfSectors) {
  const WalOptions options{.segment_bytes = 1U << 16U};
  const std::vector<Entry> a = run(1, 1, 30);
  const std::vector<Entry> b = run(2, 31, 150);
  const Images image = images(a, b, options);
  Rng rng(7);
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<std::byte> crashed = image.synced;
    for (std::uint64_t at = image.synced_end / kSector * kSector; at < image.full_end;
         at += kSector) {
      if (rng.below(4) != 0) {
        std::copy_n(image.full.begin() + static_cast<std::ptrdiff_t>(at), kSector,
                    crashed.begin() + static_cast<std::ptrdiff_t>(at));
      }
    }
    SCOPED_TRACE(trial);
    expect_recovers(crashed, a, b, options);
  }
}

TEST(WalCrash, LostTailBeforeAPersistedNextSegmentDropsTheNextSegment) {
  const TempDir dir;
  const WalOptions options{.segment_bytes = 4096};
  const std::vector<Entry> a = run(1, 1, 20);
  {
    Wal wal(dir.path(), options);
    wal.append(a);
    wal.sync();
    wal.append(run(1, 21, 200));
    wal.sync();
  }
  ASSERT_TRUE(std::filesystem::exists(segment_path(dir.path(), 3)));
  std::vector<std::byte> first = read_file(segment_path(dir.path(), 1));
  std::fill(first.begin() + 2048, first.end(), std::byte{0});
  write_bytes(segment_path(dir.path(), 1), first);
  Wal wal(dir.path(), options);
  const std::vector<Entry> entries = wal.take_entries();
  EXPECT_GT(wal.truncated(), 0U);
  EXPECT_FALSE(std::filesystem::exists(segment_path(dir.path(), 2)));
  ASSERT_GE(entries.size(), a.size());
  EXPECT_TRUE(std::equal(entries.begin(), entries.end(), run(1, 1, entries.size()).begin()));
}

TEST(WalCrash, FlippedBitInSyncedDataFailsStop) {
  const TempDir dir;
  {
    Wal wal(dir.path());
    wal.append(run(1, 1, 50));
    wal.sync();
  }
  std::vector<std::byte> bytes = read_file(segment_path(dir.path(), 1));
  bytes[700] ^= std::byte{0x04};
  write_bytes(segment_path(dir.path(), 1), bytes);
  EXPECT_THROW(Wal{dir.path()}, std::runtime_error);
}

}  // namespace
}  // namespace rme::io
