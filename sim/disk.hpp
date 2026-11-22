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

  Rng rng_;
  Image written_;
  Image durable_;
  bool state_dirty_ = false;
  std::optional<std::size_t> dirty_from_;
};

static_assert(consensus::Storage<Disk>);

}  // namespace rme::sim
