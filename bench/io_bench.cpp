#include <benchmark/benchmark.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "consensus/types.hpp"
#include "io/disk_storage.hpp"
#include "io/storage_thread.hpp"

namespace {

using rme::consensus::Entry;
using rme::consensus::EntryType;
using rme::consensus::Index;
using rme::consensus::Term;

constexpr std::uint64_t kEntries = 4096;
constexpr std::uint64_t kSyncedEntries = 512;

class ScratchDir {
 public:
  ScratchDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "rme-bench-XXXXXX").string();
    path_ = ::mkdtemp(pattern.data());
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  ScratchDir(ScratchDir&&) = delete;
  ScratchDir& operator=(ScratchDir&&) = delete;
  ~ScratchDir() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

Entry entry(std::uint64_t index) {
  return Entry{Term(1), Index(index), EntryType::kCommand, std::vector<std::byte>(256)};
}

void report(benchmark::State& state, std::uint64_t entries, std::uint64_t syncs) {
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(entries));
  state.counters["syncs_per_second"] =
      benchmark::Counter(static_cast<double>(syncs), benchmark::Counter::kIsRate);
}

void sync_per_entry(benchmark::State& state) {
  std::uint64_t syncs = 0;
  for (auto _ : state) {
    const ScratchDir dir;
    rme::io::DiskStorage storage(dir.path());
    for (std::uint64_t i = 1; i <= kSyncedEntries; ++i) {
      const std::vector<Entry> one{entry(i)};
      storage.append(one);
      storage.sync();
    }
    syncs += storage.wal_stats().syncs;
  }
  report(state, kSyncedEntries, syncs);
}

void group_commit(benchmark::State& state) {
  std::uint64_t syncs = 0;
  for (auto _ : state) {
    const ScratchDir dir;
    rme::io::DiskStorage storage(dir.path());
    {
      rme::io::StorageThread thread(storage);
      for (std::uint64_t i = 1; i <= kEntries; ++i) {
        rme::io::StorageWork work;
        work.seq = i;
        work.entries.push_back(entry(i));
        thread.submit(std::move(work));
      }
    }
    syncs += storage.wal_stats().syncs;
  }
  report(state, kEntries, syncs);
}

}  // namespace

BENCHMARK(sync_per_entry)->Unit(benchmark::kMillisecond)->UseRealTime()->Iterations(2);
BENCHMARK(group_commit)->Unit(benchmark::kMillisecond)->UseRealTime()->Iterations(2);

BENCHMARK_MAIN();
