#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "consensus/concepts.hpp"
#include "consensus/types.hpp"
#include "core/rng.hpp"

namespace rme::sim {

enum class Crash : std::uint8_t { kClean, kTornTail };

// Writes land in a volatile image; sync makes the whole image durable. A clean crash keeps only
// what was synced. A torn crash also keeps a seeded strict prefix of the unsynced writes, taken in
// the order process_ready issues them: the saved state, then the appended entries.
class Disk {
 public:
  explicit Disk(std::uint64_t seed) : rng_(seed) {}

  void save(const consensus::HardState& state);
  void append(std::span<const consensus::Entry> entries);
  void sync();
  void crash(Crash mode);
  // The next sync crashes instead of completing, as power loss during fsync would.
  void crash_on_next_sync(Crash mode) noexcept { armed_ = mode; }
  [[nodiscard]] bool crashed() const noexcept { return crashed_; }
  void restarted() noexcept { crashed_ = false; }
  // Flips a bit in a synced entry without updating its checksum.
  void corrupt(consensus::Index index);
  // The first synced entry whose checksum no longer matches.
  [[nodiscard]] std::optional<consensus::Index> corrupted() const;

  [[nodiscard]] const consensus::HardState& state() const noexcept { return durable_.state; }
  [[nodiscard]] const std::vector<consensus::Entry>& entries() const noexcept {
    return durable_.entries;
  }
  [[nodiscard]] bool dirty() const noexcept { return state_dirty_ || dirty_from_.has_value(); }

 private:
  struct Image {
    consensus::HardState state;
    std::vector<consensus::Entry> entries;
  };

  void tear();
  void persist(std::size_t from, std::span<const consensus::Entry> fresh);

  Rng rng_;
  Image written_;
  Image durable_;
  std::vector<std::uint64_t> sums_;
  bool state_dirty_ = false;
  std::optional<std::size_t> dirty_from_;
  std::optional<Crash> armed_;
  bool crashed_ = false;
};

static_assert(consensus::Storage<Disk>);

}  // namespace rme::sim
