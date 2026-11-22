#include "sim/disk.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "consensus/types.hpp"

namespace rme::sim {
namespace {

using consensus::Entry;
using consensus::EntryType;
using consensus::HardState;
using consensus::Index;
using consensus::NodeId;
using consensus::Term;

std::vector<Entry> batch(std::uint64_t first, std::uint64_t count, std::uint64_t term) {
  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint64_t index = first; index < first + count; ++index) {
    entries.push_back(
        Entry{Term(term), Index(index), EntryType::kCommand, {static_cast<std::byte>(index)}});
  }
  return entries;
}

std::vector<Entry> concat(std::vector<Entry> head, const std::vector<Entry>& tail) {
  head.insert(head.end(), tail.begin(), tail.end());
  return head;
}

TEST(DiskTest, WritesStayVolatileUntilSync) {
  Disk disk(1);
  disk.save(HardState{Term(3), NodeId(2)});
  disk.append(batch(1, 4, 3));
  EXPECT_TRUE(disk.dirty());
  EXPECT_EQ(disk.state(), HardState{});
  EXPECT_TRUE(disk.entries().empty());

  disk.sync();
  EXPECT_FALSE(disk.dirty());
  EXPECT_EQ(disk.state(), (HardState{Term(3), NodeId(2)}));
  EXPECT_EQ(disk.entries(), batch(1, 4, 3));
}

TEST(DiskTest, AppendReplacesTheSuffix) {
  Disk disk(1);
  disk.append(batch(1, 5, 1));
  disk.sync();
  disk.append(batch(3, 2, 2));
  EXPECT_EQ(disk.entries(), batch(1, 5, 1));
  disk.sync();
  EXPECT_EQ(disk.entries(), concat(batch(1, 2, 1), batch(3, 2, 2)));
}

TEST(DiskTest, CrashLosesUnsyncedVote) {
  Disk disk(1);
  disk.save(HardState{Term(2), NodeId(1)});
  disk.sync();
  disk.save(HardState{Term(5), NodeId(3)});
  disk.crash(Crash::kClean);
  EXPECT_EQ(disk.state(), (HardState{Term(2), NodeId(1)}));
  disk.sync();
  EXPECT_EQ(disk.state(), (HardState{Term(2), NodeId(1)}));
}

TEST(DiskTest, CrashLosesUnsyncedEntriesAndKeepsSyncedOnes) {
  Disk disk(1);
  disk.append(batch(1, 3, 1));
  disk.sync();
  disk.append(batch(4, 3, 1));
  disk.crash(Crash::kClean);
  EXPECT_EQ(disk.entries(), batch(1, 3, 1));
  EXPECT_FALSE(disk.dirty());
  disk.sync();
  EXPECT_EQ(disk.entries(), batch(1, 3, 1));
}

TEST(DiskTest, CrashUndoesAnUnsyncedTruncation) {
  Disk disk(1);
  disk.append(batch(1, 5, 1));
  disk.sync();
  disk.append(batch(3, 1, 2));
  disk.crash(Crash::kClean);
  EXPECT_EQ(disk.entries(), batch(1, 5, 1));
}

TEST(DiskTest, TornCrashKeepsOnlyAStrictPrefixOfUnsyncedWrites) {
  const HardState old_state{Term(1), NodeId(1)};
  const HardState new_state{Term(2), NodeId(2)};
  const std::vector<Entry> written = concat(batch(1, 2, 1), batch(3, 8, 2));
  std::set<std::size_t> lengths;
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    Disk disk(seed);
    disk.save(old_state);
    disk.append(batch(1, 2, 1));
    disk.sync();
    disk.save(new_state);
    disk.append(batch(3, 8, 2));
    disk.crash(Crash::kTornTail);

    const std::vector<Entry>& kept = disk.entries();
    ASSERT_GE(kept.size(), 2U);
    ASSERT_LT(kept.size(), written.size());
    EXPECT_TRUE(std::equal(kept.begin(), kept.end(), written.begin()));
    if (kept.size() > 2) {
      EXPECT_EQ(disk.state(), new_state);
    }
    lengths.insert(kept.size());
  }
  EXPECT_EQ(lengths.size(), 8U);
}

TEST(DiskTest, TornCrashWithNothingUnsyncedKeepsEverything) {
  Disk disk(1);
  disk.save(HardState{Term(4), NodeId(2)});
  disk.append(batch(1, 3, 4));
  disk.sync();
  disk.crash(Crash::kTornTail);
  EXPECT_EQ(disk.state(), (HardState{Term(4), NodeId(2)}));
  EXPECT_EQ(disk.entries(), batch(1, 3, 4));
}

TEST(DiskDeathTest, AppendWithAGapAsserts) {
  Disk disk(1);
  EXPECT_DEATH(disk.append(batch(2, 1, 1)), "from <= written_.entries.size()");
}

}  // namespace
}  // namespace rme::sim
