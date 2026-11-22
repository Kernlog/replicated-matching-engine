#include "sim/disk.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

#include "core/assert.hpp"

namespace rme::sim {

void Disk::save(const consensus::HardState& state) {
  written_.state = state;
  state_dirty_ = true;
}

void Disk::append(std::span<const consensus::Entry> entries) {
  RME_ASSERT(!entries.empty());
  const auto from = static_cast<std::size_t>(entries.front().index.value() - 1);
  RME_ASSERT(from <= written_.entries.size());
  written_.entries.resize(from);
  written_.entries.insert(written_.entries.end(), entries.begin(), entries.end());
  dirty_from_ = std::min(dirty_from_.value_or(from), from);
}

void Disk::sync() {
  durable_.state = written_.state;
  const std::size_t from = dirty_from_.value_or(written_.entries.size());
  durable_.entries.resize(from);
  const auto fresh = std::span(written_.entries).subspan(from);
  durable_.entries.insert(durable_.entries.end(), fresh.begin(), fresh.end());
  state_dirty_ = false;
  dirty_from_.reset();
}

}  // namespace rme::sim
