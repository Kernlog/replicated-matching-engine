#include "sim/disk.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/assert.hpp"
#include "core/hash.hpp"

namespace rme::sim {

namespace {

std::uint64_t checksum(const consensus::Entry& entry) {
  Hasher hasher;
  hasher.add(entry.term.value());
  hasher.add(entry.index.value());
  hasher.add(static_cast<std::uint64_t>(entry.type));
  hasher.add(entry.data.size());
  for (const std::byte byte : entry.data) {
    hasher.add(static_cast<std::uint64_t>(byte));
  }
  return hasher.digest();
}

}  // namespace

void Disk::save(const consensus::HardState& state) {
  written_.state = state;
  state_dirty_ = true;
}

void Disk::save_snapshot(const consensus::Snapshot& snapshot) {
  const std::size_t at = snapshot.index.value();
  if (written_.entries.size() < at || written_.entries[at - 1].term != snapshot.term) {
    const std::size_t keep = std::min(written_.entries.size(), at);
    written_.entries.resize(at, consensus::Entry{});
    for (std::size_t i = keep; i < at; ++i) {
      written_.entries[i].index = consensus::Index(i + 1);
    }
    dirty_from_ = std::min(dirty_from_.value_or(keep), keep);
  }
  written_.snapshot = snapshot;
  snapshot_dirty_ = true;
}

std::vector<consensus::Entry> Disk::log() const {
  const std::size_t from = durable_.snapshot ? durable_.snapshot->index.value() : 0;
  const auto begin = durable_.entries.begin() +
                     static_cast<std::ptrdiff_t>(std::min(from, durable_.entries.size()));
  return {begin, durable_.entries.end()};
}

void Disk::append(std::span<const consensus::Entry> entries) {
  RME_ASSERT(!entries.empty());
  const auto from = static_cast<std::size_t>(entries.front().index.value() - 1);
  const std::size_t compacted = written_.snapshot ? written_.snapshot->index.value() : 0;
  RME_ASSERT(from <= std::max(written_.entries.size(), compacted));
  for (std::size_t i = written_.entries.size(); i < from; ++i) {
    consensus::Entry placeholder;
    placeholder.index = consensus::Index(i + 1);
    written_.entries.push_back(std::move(placeholder));
  }
  written_.entries.resize(from);
  written_.entries.insert(written_.entries.end(), entries.begin(), entries.end());
  dirty_from_ = std::min(dirty_from_.value_or(from), from);
}

void Disk::sync() {
  if (armed_) {
    crash(*armed_);
    armed_.reset();
    crashed_ = true;
    return;
  }
  durable_.state = written_.state;
  durable_.snapshot = written_.snapshot;
  const std::size_t from = dirty_from_.value_or(written_.entries.size());
  persist(from, std::span(written_.entries).subspan(from));
  state_dirty_ = false;
  snapshot_dirty_ = false;
  dirty_from_.reset();
}

void Disk::crash(Crash mode) {
  if (mode == Crash::kTornTail) {
    tear();
  }
  written_ = durable_;
  state_dirty_ = false;
  snapshot_dirty_ = false;
  dirty_from_.reset();
}

void Disk::tear() {
  const std::size_t from = dirty_from_.value_or(written_.entries.size());
  const std::size_t writes =
      (state_dirty_ ? 1U : 0U) + (snapshot_dirty_ ? 1U : 0U) + (written_.entries.size() - from);
  if (writes == 0) {
    return;
  }
  std::size_t kept = rng_.below(writes);
  if (kept > 0 && state_dirty_) {
    durable_.state = written_.state;
    --kept;
  }
  if (kept > 0 && snapshot_dirty_) {
    durable_.snapshot = written_.snapshot;
    --kept;
  }
  if (kept > 0) {
    persist(from, std::span(written_.entries).subspan(from, kept));
  }
}

void Disk::persist(std::size_t from, std::span<const consensus::Entry> fresh) {
  durable_.entries.resize(from);
  durable_.entries.insert(durable_.entries.end(), fresh.begin(), fresh.end());
  sums_.resize(from);
  sums_.reserve(durable_.entries.size());
  for (const consensus::Entry& entry : fresh) {
    sums_.push_back(checksum(entry));
  }
}

void Disk::corrupt(consensus::Index index) {
  RME_ASSERT(index > consensus::Index(0) && index.value() <= durable_.entries.size());
  consensus::Entry& entry = durable_.entries[index.value() - 1];
  entry.term = consensus::Term(entry.term.value() ^ 1U);
}

std::optional<consensus::Index> Disk::corrupted() const {
  for (std::size_t i = 0; i < durable_.entries.size(); ++i) {
    if (checksum(durable_.entries[i]) != sums_[i]) {
      return consensus::Index(i + 1);
    }
  }
  return std::nullopt;
}

}  // namespace rme::sim
