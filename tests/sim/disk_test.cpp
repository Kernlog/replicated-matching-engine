#include "sim/disk.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
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
  Disk disk;
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
  Disk disk;
  disk.append(batch(1, 5, 1));
  disk.sync();
  disk.append(batch(3, 2, 2));
  EXPECT_EQ(disk.entries(), batch(1, 5, 1));
  disk.sync();
  EXPECT_EQ(disk.entries(), concat(batch(1, 2, 1), batch(3, 2, 2)));
}

TEST(DiskDeathTest, AppendWithAGapAsserts) {
  Disk disk;
  EXPECT_DEATH(disk.append(batch(2, 1, 1)), "from <= written_.entries.size()");
}

}  // namespace
}  // namespace rme::sim
