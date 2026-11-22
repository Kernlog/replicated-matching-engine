#include "log/command_log.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <system_error>
#include <vector>

#include "codec/record.hpp"
#include "testing/command_stream.hpp"

namespace rme::log {
namespace {

using engine::Command;

class CommandLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::filesystem::path(::testing::TempDir()) /
            (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".log");
    std::filesystem::remove(path_);
  }
  void TearDown() override { std::filesystem::remove(path_); }

  std::vector<Command> write(std::size_t count, std::uint64_t seed = 1) {
    testing::CommandStream stream(seed);
    std::vector<Command> commands;
    LogWriter writer(path_);
    for (std::size_t i = 0; i < count; ++i) {
      commands.push_back(stream.next());
      writer.append(commands.back());
    }
    writer.sync();
    return commands;
  }

  std::vector<Command> read_all(std::uint64_t* valid = nullptr) {
    LogReader reader(path_);
    std::vector<Command> out;
    while (auto command = reader.next()) {
      out.push_back(*command);
    }
    if (valid != nullptr) {
      *valid = reader.valid_bytes();
    }
    return out;
  }

  std::filesystem::path path_;
};

TEST_F(CommandLogTest, ReadsBackWhatWasAppended) {
  const auto written = write(40'000);
  std::uint64_t valid = 0;
  EXPECT_EQ(read_all(&valid), written);
  EXPECT_EQ(valid, written.size() * codec::kRecordSize);
  EXPECT_EQ(std::filesystem::file_size(path_), valid);
}

TEST_F(CommandLogTest, EmptyLogHasNoRecords) {
  write(0);
  EXPECT_TRUE(read_all().empty());
}

TEST_F(CommandLogTest, ReopenAppendsAfterExistingRecords) {
  auto expected = write(5, 1);
  const auto more = write(5, 2);
  expected.insert(expected.end(), more.begin(), more.end());
  EXPECT_EQ(read_all(), expected);
}

TEST_F(CommandLogTest, StopsAtTornTail) {
  auto written = write(100);
  std::filesystem::resize_file(path_, 100 * codec::kRecordSize - 7);
  std::uint64_t valid = 0;
  written.pop_back();
  EXPECT_EQ(read_all(&valid), written);
  EXPECT_EQ(valid, 99 * codec::kRecordSize);
}

TEST_F(CommandLogTest, StopsAtCorruptRecord) {
  auto written = write(100);
  {
    std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(50 * codec::kRecordSize + 12);
    file.put('\x5A');
  }
  written.erase(written.begin() + 50, written.end());
  EXPECT_EQ(read_all(), written);
}

TEST_F(CommandLogTest, WriterTruncatesTornTailBeforeAppending) {
  auto expected = write(10, 1);
  std::filesystem::resize_file(path_, 10 * codec::kRecordSize - 3);
  expected.pop_back();
  const auto more = write(3, 2);
  expected.insert(expected.end(), more.begin(), more.end());
  EXPECT_EQ(read_all(), expected);
}

TEST_F(CommandLogTest, ReaderThrowsOnMissingFile) {
  EXPECT_THROW(LogReader{path_}, std::system_error);
}

}  // namespace
}  // namespace rme::log
