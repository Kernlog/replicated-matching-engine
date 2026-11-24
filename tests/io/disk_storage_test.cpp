#include "io/disk_storage.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "core/rng.hpp"
#include "tests/consensus/memory_storage.hpp"
#include "tests/io/temp_dir.hpp"

namespace rme::io {
namespace {

using consensus::Entry;
using consensus::EntryType;
using consensus::HardState;
using consensus::Index;
using consensus::MemoryStorage;
using consensus::NodeId;
using consensus::Snapshot;
using consensus::Term;

void expect_same(const Durable& disk, const MemoryStorage& memory) {
  EXPECT_EQ(disk.state, memory.state());
  EXPECT_EQ(disk.snapshot, memory.snapshot());
  EXPECT_EQ(disk.log, memory.log());
}

TEST(DiskStorage, FreshDirectoryIsEmpty) {
  const TempDir dir;
  DiskStorage storage(dir.path());
  const Durable durable = storage.take_recovered();
  EXPECT_EQ(durable.state, HardState{});
  EXPECT_FALSE(durable.snapshot.has_value());
  EXPECT_TRUE(durable.log.empty());
}

TEST(DiskStorage, UnsyncedSavesAreLostOnReopen) {
  const TempDir dir;
  {
    DiskStorage storage(dir.path());
    storage.save(HardState{.term = Term(2), .vote = NodeId(3)});
    storage.append(std::vector{Entry{Term(2), Index(1), EntryType::kNoop, {}}});
    storage.sync();
    storage.save(HardState{.term = Term(5), .vote = std::nullopt});
    storage.append(std::vector{Entry{Term(5), Index(2), EntryType::kNoop, {}}});
  }
  DiskStorage storage(dir.path());
  const Durable durable = storage.take_recovered();
  EXPECT_EQ(durable.state, (HardState{.term = Term(2), .vote = NodeId(3)}));
  EXPECT_EQ(durable.log.size(), 1U);
}

TEST(DiskStorage, SyncWithoutChangesWritesNothing) {
  const TempDir dir;
  DiskStorage storage(dir.path());
  storage.save(HardState{});
  storage.sync();
  EXPECT_EQ(storage.wal_stats().syncs, 0U);
}

// Random save, save_snapshot, append and sync sequences reopen to exactly what MemoryStorage holds.
TEST(DiskStorage, MatchesMemoryStorageAcrossRandomReopens) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    const TempDir dir;
    Rng rng(seed);
    MemoryStorage memory;
    std::optional<DiskStorage> disk(std::in_place, dir.path(), WalOptions{.segment_bytes = 8192});
    std::uint64_t term = 1;
    std::uint64_t base = 0;
    std::uint64_t last = 0;
    std::vector<std::uint64_t> terms{0};
    for (int step = 0; step < 300; ++step) {
      const std::uint64_t roll = rng.below(10);
      if (roll < 2) {
        term += rng.below(2);
        const HardState state{
            .term = Term(term),
            .vote = rng.below(2) == 0
                        ? std::nullopt
                        : std::optional(NodeId(static_cast<std::uint32_t>(1 + rng.below(3))))};
        memory.save(state);
        disk->save(state);
      } else if (roll < 7) {
        const std::uint64_t from = base + 1 + rng.below(last - base + 1);
        std::vector<Entry> entries;
        terms.resize(from);
        for (std::uint64_t i = from; i < from + 1 + rng.below(8); ++i) {
          std::vector<std::byte> data(rng.below(40), static_cast<std::byte>(i));
          entries.push_back(Entry{Term(term), Index(i), EntryType::kCommand, std::move(data)});
          terms.push_back(term);
        }
        last = entries.back().index.value();
        memory.append(entries);
        disk->append(entries);
      } else if (roll < 8) {
        const std::uint64_t at = base + 1 + rng.below(last - base + 3);
        const bool matches = at <= last && rng.below(2) == 0;
        const Snapshot snapshot{.index = Index(at),
                                .term = Term(matches ? terms[at] : term + 1),
                                .data = std::vector<std::byte>(rng.below(64), std::byte{7})};
        memory.sync();
        disk->sync();
        memory.save_snapshot(snapshot);
        disk->save_snapshot(snapshot);
        memory.sync();
        disk->sync();
        base = at;
        if (!matches || at > last) {
          last = at;
          terms.assign(at + 1, 0);
        }
      } else {
        memory.sync();
        disk->sync();
        if (rng.below(3) == 0) {
          disk.emplace(dir.path(), WalOptions{.segment_bytes = 8192});
          expect_same(disk->take_recovered(), memory);
        }
      }
    }
    memory.sync();
    disk->sync();
    disk.emplace(dir.path(), WalOptions{.segment_bytes = 8192});
    expect_same(disk->take_recovered(), memory);
  }
}

}  // namespace
}  // namespace rme::io
