#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "consensus/concepts.hpp"
#include "consensus/types.hpp"
#include "io/wal.hpp"

namespace rme::io {

struct Durable {
  consensus::HardState state;
  std::vector<consensus::Entry> log;
  std::optional<consensus::Snapshot> snapshot;
};

// Raft storage on disk: hard state and snapshot in files replaced atomically, entries in the WAL.
// Saves are held until sync, which writes the hard state before any entry, so a crash can never
// keep an entry from a term the node has not durably entered.
class DiskStorage {
 public:
  explicit DiskStorage(const std::filesystem::path& dir, WalOptions wal = {});

  // What the last sync made durable, as a restarting node rebuilds from it.
  [[nodiscard]] Durable take_recovered() noexcept { return std::move(recovered_); }

  void save(const consensus::HardState& state);
  void save_snapshot(const consensus::Snapshot& snapshot);
  void append(std::span<const consensus::Entry> entries);
  void sync();

  [[nodiscard]] const WalStats& wal_stats() const noexcept { return wal_.stats(); }

 private:
  std::filesystem::path hard_state_path_;
  std::filesystem::path snapshot_path_;
  Wal wal_;
  Durable recovered_;
  consensus::HardState state_;
  std::optional<consensus::HardState> pending_state_;
  std::optional<consensus::Snapshot> pending_snapshot_;
  std::vector<consensus::Entry> pending_entries_;
};

static_assert(consensus::Storage<DiskStorage>);

}  // namespace rme::io
