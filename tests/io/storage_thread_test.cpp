#include "io/storage_thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/spsc.hpp"
#include "tests/io/temp_dir.hpp"

namespace rme::io {
namespace {

using consensus::Entry;
using consensus::EntryType;
using consensus::HardState;
using consensus::Index;
using consensus::Term;

StorageWork work(std::uint64_t seq) {
  StorageWork out;
  out.seq = seq;
  out.entries.push_back(Entry{Term(1), Index(seq), EntryType::kCommand,
                              std::vector<std::byte>(48, static_cast<std::byte>(seq))});
  return out;
}

TEST(StorageThread, SharesOneSyncAcrossEverythingQueuedMeanwhile) {
  const TempDir dir;
  constexpr std::uint64_t kWorks = 5000;
  {
    DiskStorage storage(dir.path());
    std::atomic<int> wakeups{0};
    StorageThread thread(storage, [&wakeups] { wakeups.fetch_add(1, std::memory_order_relaxed); });
    for (std::uint64_t seq = 1; seq <= kWorks; ++seq) {
      thread.submit(work(seq));
    }
    while (thread.synced() != kWorks) {
      cpu_relax();
    }
    EXPECT_GE(wakeups.load(), 1);
  }
  DiskStorage storage(dir.path());
  const Durable durable = storage.take_recovered();
  ASSERT_EQ(durable.log.size(), kWorks);
  EXPECT_EQ(durable.log.back().index, Index(kWorks));
}

TEST(StorageThread, SyncsFewerTimesThanItReceivesWork) {
  const TempDir dir;
  DiskStorage storage(dir.path());
  {
    StorageThread thread(storage);
    for (std::uint64_t seq = 1; seq <= 2000; ++seq) {
      thread.submit(work(seq));
    }
  }
  EXPECT_LT(storage.wal_stats().syncs, 2000U / 4);
}

TEST(StorageThread, HardStateAndSnapshotArriveInOrder) {
  const TempDir dir;
  {
    DiskStorage storage(dir.path());
    StorageThread thread(storage);
    StorageWork first = work(1);
    first.state = HardState{.term = Term(4), .vote = std::nullopt};
    thread.submit(std::move(first));
    thread.submit(work(2));
    StorageWork snap;
    snap.seq = 3;
    snap.snapshot = consensus::Snapshot{.index = Index(2), .term = Term(1), .data = {std::byte{1}}};
    thread.submit(std::move(snap));
    thread.submit(work(3));
  }
  DiskStorage storage(dir.path());
  const Durable durable = storage.take_recovered();
  EXPECT_EQ(durable.state.term, Term(4));
  ASSERT_TRUE(durable.snapshot.has_value());
  EXPECT_EQ(durable.snapshot.value_or(consensus::Snapshot{}).index, Index(2));
  ASSERT_EQ(durable.log.size(), 1U);
  EXPECT_EQ(durable.log.front().index, Index(3));
}

}  // namespace
}  // namespace rme::io
