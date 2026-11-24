#include "io/storage_thread.hpp"

#include <array>
#include <utility>

#include "core/assert.hpp"

namespace rme::io {

namespace {

constexpr std::size_t kBatch = 256;

}  // namespace

StorageThread::StorageThread(DiskStorage& storage, std::function<void()> on_synced,
                             std::size_t capacity)
    : storage_(storage),
      on_synced_(std::move(on_synced)),
      queue_(capacity),
      thread_([this] { run(); }) {}

StorageThread::~StorageThread() {
  queue_.push(StorageWork{});
  thread_.join();
}

void StorageThread::submit(StorageWork&& work) noexcept {
  RME_ASSERT(work.seq != 0);
  queue_.push(std::move(work));
}

void StorageThread::run() {
  std::vector<StorageWork> batch(kBatch);
  for (;;) {
    queue_.wait();
    const std::size_t count = queue_.pop_n(batch);
    std::uint64_t last = 0;
    bool pending = false;
    bool stop = false;
    for (std::size_t i = 0; i < count; ++i) {
      StorageWork& work = batch[i];
      if (work.seq == 0) {
        stop = true;
        continue;
      }
      if (work.state) {
        storage_.save(*work.state);
      }
      if (work.snapshot) {
        if (pending) {
          storage_.sync();
          ++stats_.syncs;
        }
        storage_.save_snapshot(*work.snapshot);
      }
      if (!work.entries.empty()) {
        storage_.append(work.entries);
      }
      pending = true;
      last = work.seq;
      ++stats_.works;
      work = StorageWork{};
    }
    if (pending) {
      storage_.sync();
      ++stats_.syncs;
      synced_.store(last, std::memory_order_release);
      if (on_synced_) {
        on_synced_();
      }
    }
    if (stop) {
      return;
    }
  }
}

}  // namespace rme::io
