#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

#include "consensus/types.hpp"
#include "core/spsc.hpp"
#include "io/disk_storage.hpp"

namespace rme::io {

// One Ready's worth of durable state, tagged with the sequence number the core waits on.
struct StorageWork {
  std::uint64_t seq = 0;
  std::optional<consensus::HardState> state;
  std::optional<consensus::Snapshot> snapshot;
  std::vector<consensus::Entry> entries;
};

struct GroupCommitStats {
  std::uint64_t works = 0;
  std::uint64_t syncs = 0;
};

// Owns the disk on its own thread. It drains every queued work, makes the whole batch durable with
// one sync, then publishes the batch's last sequence number, so fsync cost is shared by everything
// that arrived while the previous sync ran.
class StorageThread {
 public:
  StorageThread(DiskStorage& storage, std::function<void()> on_synced = {},
                std::size_t capacity = 1024);
  StorageThread(const StorageThread&) = delete;
  StorageThread& operator=(const StorageThread&) = delete;
  StorageThread(StorageThread&&) = delete;
  StorageThread& operator=(StorageThread&&) = delete;
  // Finishes and syncs everything already submitted. Runs on the submitting thread.
  ~StorageThread();

  // Core thread only. seq must increase with every call.
  void submit(StorageWork&& work) noexcept;
  // Every work with a sequence number at or below this is durable.
  [[nodiscard]] std::uint64_t synced() const noexcept {
    return synced_.load(std::memory_order_acquire);
  }
  // Valid once the thread has stopped.
  [[nodiscard]] const GroupCommitStats& stats() const noexcept { return stats_; }

 private:
  void run();
  void stage(const StorageWork& work, bool pending);

  DiskStorage& storage_;
  std::function<void()> on_synced_;
  SpscQueue<StorageWork> queue_;
  alignas(kFalseSharingRange) std::atomic<std::uint64_t> synced_{0};
  GroupCommitStats stats_;
  std::jthread thread_;
};

}  // namespace rme::io
