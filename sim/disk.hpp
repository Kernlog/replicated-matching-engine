#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "consensus/concepts.hpp"
#include "consensus/types.hpp"

namespace rme::sim {

// Writes land in a volatile image; sync makes the whole image durable.
class Disk {
 public:
  void save(const consensus::HardState& state);
  void append(std::span<const consensus::Entry> entries);
  void sync();

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

  Image written_;
  Image durable_;
  bool state_dirty_ = false;
  std::optional<std::size_t> dirty_from_;
};

static_assert(consensus::Storage<Disk>);

}  // namespace rme::sim
