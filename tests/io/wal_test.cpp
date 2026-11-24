#include "io/wal.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "tests/io/temp_dir.hpp"

namespace rme::io {
namespace {

using consensus::Entry;
using consensus::EntryType;
using consensus::Index;
using consensus::Term;

Entry entry(std::uint64_t term, std::uint64_t index, std::size_t size = 11) {
  std::vector<std::byte> data(size);
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<std::byte>(index * 7 + i);
  }
  return Entry{.term = Term(term),
               .index = Index(index),
               .type = EntryType::kCommand,
               .data = std::move(data)};
}

std::vector<Entry> run(std::uint64_t term, std::uint64_t from, std::uint64_t to) {
  std::vector<Entry> out;
  for (std::uint64_t i = from; i <= to; ++i) {
    out.push_back(entry(term, i, i % 13));
  }
  return out;
}

std::size_t segment_count(const std::filesystem::path& dir) {
  std::size_t count = 0;
  for (const auto& item : std::filesystem::directory_iterator(dir)) {
    count += item.path().extension() == ".wal" ? 1U : 0U;
  }
  return count;
}

TEST(Wal, ReopenReadsBackWhatWasSynced) {
  const TempDir dir;
  const std::vector<Entry> written = run(1, 1, 100);
  {
    Wal wal(dir.path());
    wal.append(written);
    wal.sync();
    EXPECT_EQ(wal.stats().writes, 1U);
    EXPECT_EQ(wal.stats().syncs, 1U);
  }
  Wal wal(dir.path());
  EXPECT_EQ(wal.take_entries(), written);
}

TEST(Wal, PreallocatesSegmentsToTheirFullSize) {
  const TempDir dir;
  const Wal wal(dir.path(), {.segment_bytes = 1U << 16U});
  EXPECT_EQ(std::filesystem::file_size(segment_path(dir.path(), 1)), 1U << 16U);
}

TEST(Wal, LowerIndexReplacesTheSuffix) {
  const TempDir dir;
  {
    Wal wal(dir.path());
    wal.append(run(1, 1, 10));
    wal.append(run(2, 6, 8));
    wal.sync();
  }
  std::vector<Entry> expected = run(1, 1, 5);
  for (Entry& e : run(2, 6, 8)) {
    expected.push_back(std::move(e));
  }
  Wal wal(dir.path());
  EXPECT_EQ(wal.take_entries(), expected);
}

TEST(Wal, SnapshotMarkKeepsTheSuffixOnlyWhenTheTermMatches) {
  const TempDir dir;
  {
    Wal wal(dir.path());
    wal.append(run(3, 1, 20));
    wal.mark_snapshot(Index(12), Term(3));
    wal.sync();
  }
  {
    Wal wal(dir.path());
    EXPECT_EQ(wal.mark_index(), Index(12));
    EXPECT_EQ(wal.take_entries(), run(3, 13, 20));
    wal.mark_snapshot(Index(30), Term(4));
    wal.append(run(4, 31, 32));
    wal.sync();
  }
  Wal wal(dir.path());
  EXPECT_EQ(wal.mark_index(), Index(30));
  EXPECT_EQ(wal.mark_term(), Term(4));
  EXPECT_EQ(wal.take_entries(), run(4, 31, 32));
}

TEST(Wal, RollsSegmentsAndReplaysAcrossThem) {
  const TempDir dir;
  const std::vector<Entry> written = run(1, 1, 400);
  {
    Wal wal(dir.path(), {.segment_bytes = 4096});
    wal.append(std::span(written).first(150));
    wal.sync();
    wal.append(std::span(written).subspan(150));
    wal.sync();
  }
  EXPECT_GE(segment_count(dir.path()), 4U);
  Wal wal(dir.path(), {.segment_bytes = 4096});
  EXPECT_EQ(wal.take_entries(), written);
}

TEST(Wal, ContinuesTheLastSegmentAfterReopen) {
  const TempDir dir;
  {
    Wal wal(dir.path());
    wal.append(run(1, 1, 3));
    wal.sync();
  }
  {
    Wal wal(dir.path());
    wal.append(run(1, 4, 6));
    wal.sync();
  }
  Wal wal(dir.path());
  EXPECT_EQ(segment_count(dir.path()), 1U);
  EXPECT_EQ(wal.take_entries(), run(1, 1, 6));
}

TEST(Wal, RejectsAGapInIndices) {
  const TempDir dir;
  {
    Wal wal(dir.path());
    wal.append(run(1, 1, 3));
    wal.append(run(1, 5, 5));
    wal.sync();
  }
  EXPECT_THROW(Wal{dir.path()}, std::runtime_error);
}

}  // namespace
}  // namespace rme::io
