#include <benchmark/benchmark.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "consensus/types.hpp"
#include "io/disk_storage.hpp"
#include "io/storage_thread.hpp"
#include "io/transport.hpp"

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

rme::consensus::Message append(std::uint64_t index) {
  return rme::consensus::Message{
      rme::consensus::NodeId(1), rme::consensus::NodeId(2), Term(1),
      rme::consensus::AppendRequest{Index(index - 1), Term(1), {entry(index)}, Index(0)}};
}

// Frames from one node to another over loopback, flushed in batches of 64 as a Ready would be.
void transport_stream(benchmark::State& state) {
  constexpr std::uint64_t kMessages = 100'000;
  constexpr std::uint64_t kWindow = 2048;
  std::atomic<std::uint64_t> received{0};
  rme::io::Transport receiver(rme::consensus::NodeId(2), "127.0.0.1", 0,
                              [&received](const rme::consensus::Message&) {
                                received.fetch_add(1, std::memory_order_relaxed);
                              });
  receiver.start();
  rme::io::Transport sender(rme::consensus::NodeId(1), "127.0.0.1", 0,
                            [](const rme::consensus::Message&) {});
  sender.add_peer({rme::consensus::NodeId(2), "127.0.0.1", receiver.port()});
  sender.start();
  while (received.load() == 0) {
    sender.send(append(1));
    sender.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::uint64_t sent = 0;
  for (auto _ : state) {
    const std::uint64_t start = received.load();
    for (std::uint64_t i = 0; i < kMessages; ++i) {
      while (sent - (received.load(std::memory_order_relaxed) - start) >= kWindow) {
        std::this_thread::yield();
      }
      sender.send(append(2 + i));
      ++sent;
      if (i % 64 == 63) {
        sender.flush();
      }
    }
    sender.flush();
    while (received.load() - start < kMessages) {
      std::this_thread::yield();
    }
    sent = 0;
  }
  sender.stop();
  const rme::io::TransportStats& stats = sender.stats();
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kMessages));
  state.counters["writes_per_kframe"] =
      1000.0 * static_cast<double>(stats.writes) / static_cast<double>(stats.frames_sent);
}

}  // namespace

BENCHMARK(transport_stream)->Unit(benchmark::kMillisecond)->UseRealTime()->Iterations(3);
BENCHMARK(sync_per_entry)->Unit(benchmark::kMillisecond)->UseRealTime()->Iterations(2);
BENCHMARK(group_commit)->Unit(benchmark::kMillisecond)->UseRealTime()->Iterations(2);

BENCHMARK_MAIN();
