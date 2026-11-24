#include "io/disk_storage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <type_traits>

#include "codec/crc32c.hpp"
#include "core/assert.hpp"
#include "io/atomic_file.hpp"
#include "io/fs.hpp"

namespace rme::io {

namespace {

struct HardStateRecord {
  std::uint64_t term = 0;
  std::uint32_t vote = 0;
  std::uint32_t crc = 0;
};

struct SnapshotHeader {
  std::uint64_t index = 0;
  std::uint64_t term = 0;
  std::uint64_t length = 0;
  std::uint32_t crc = 0;
  std::uint32_t reserved = 0;
};

static_assert(std::has_unique_object_representations_v<HardStateRecord>);
static_assert(std::has_unique_object_representations_v<SnapshotHeader>);

template <typename T>
std::uint32_t crc_of(T header, std::span<const std::byte> body = {}) noexcept {
  header.crc = 0;
  return codec::crc32c(body, codec::crc32c(std::as_bytes(std::span(&header, 1))));
}

template <typename T>
T read_header(std::span<const std::byte> bytes, const std::filesystem::path& path) {
  if (bytes.size() < sizeof(T)) {
    throw std::runtime_error("short file " + path.string());
  }
  T header;
  std::memcpy(&header, bytes.data(), sizeof header);
  return header;
}

consensus::HardState load_hard_state(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return {};
  }
  const std::vector<std::byte> bytes = read_file(path);
  const auto record = read_header<HardStateRecord>(bytes, path);
  if (record.crc != crc_of(record)) {
    throw std::runtime_error("corrupt " + path.string());
  }
  consensus::HardState state{.term = consensus::Term(record.term), .vote = std::nullopt};
  if (record.vote != 0) {
    state.vote = consensus::NodeId(record.vote);
  }
  return state;
}

std::optional<consensus::Snapshot> load_snapshot(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  const std::vector<std::byte> bytes = read_file(path);
  const auto header = read_header<SnapshotHeader>(bytes, path);
  const auto body = std::span(bytes).subspan(sizeof header);
  if (header.length != body.size() || header.crc != crc_of(header, body)) {
    throw std::runtime_error("corrupt " + path.string());
  }
  return consensus::Snapshot{.index = consensus::Index(header.index),
                             .term = consensus::Term(header.term),
                             .data = {body.begin(), body.end()}};
}

// Applies a snapshot whose WAL mark did not survive: the snapshot replaces the entries it covers,
// and the rest too unless the log holds its index at its term.
void cover(std::vector<consensus::Entry>& log, const consensus::Snapshot& snapshot) {
  const auto held = std::ranges::find_if(log, [&](const consensus::Entry& entry) {
    return entry.index == snapshot.index && entry.term == snapshot.term;
  });
  if (held == log.end()) {
    log.clear();
  } else {
    log.erase(log.begin(), held + 1);
  }
}

}  // namespace

DiskStorage::DiskStorage(const std::filesystem::path& dir, WalOptions wal)
    : hard_state_path_(dir / "hard_state"),
      snapshot_path_(dir / "snapshot"),
      wal_(dir / "wal", wal) {
  recovered_.state = load_hard_state(hard_state_path_);
  recovered_.snapshot = load_snapshot(snapshot_path_);
  recovered_.log = wal_.take_entries();
  const consensus::Index snapshot_index =
      recovered_.snapshot ? recovered_.snapshot->index : consensus::Index(0);
  if (snapshot_index < wal_.mark_index()) {
    throw std::runtime_error("wal marks a snapshot newer than " + snapshot_path_.string());
  }
  if (recovered_.snapshot && snapshot_index > wal_.mark_index()) {
    cover(recovered_.log, *recovered_.snapshot);
  }
  state_ = recovered_.state;
}

void DiskStorage::save(const consensus::HardState& state) {
  pending_state_ = state;
}

void DiskStorage::save_snapshot(const consensus::Snapshot& snapshot) {
  RME_ASSERT(pending_entries_.empty());
  pending_snapshot_ = snapshot;
}

void DiskStorage::append(std::span<const consensus::Entry> entries) {
  pending_entries_.insert(pending_entries_.end(), entries.begin(), entries.end());
}

void DiskStorage::sync() {
  if (pending_state_ && *pending_state_ != state_) {
    HardStateRecord record{.term = pending_state_->term.value(),
                           .vote = pending_state_->vote.value_or(consensus::NodeId(0)).value(),
                           .crc = 0};
    record.crc = crc_of(record);
    write_atomic(hard_state_path_, std::as_bytes(std::span(&record, 1)));
    state_ = *pending_state_;
  }
  pending_state_.reset();
  bool wrote = false;
  if (pending_snapshot_) {
    const consensus::Snapshot& snapshot = *pending_snapshot_;
    SnapshotHeader header{.index = snapshot.index.value(),
                          .term = snapshot.term.value(),
                          .length = snapshot.data.size(),
                          .crc = 0,
                          .reserved = 0};
    header.crc = crc_of(header, snapshot.data);
    std::vector<std::byte> bytes(sizeof header + snapshot.data.size());
    std::memcpy(bytes.data(), &header, sizeof header);
    std::ranges::copy(snapshot.data, bytes.begin() + static_cast<std::ptrdiff_t>(sizeof header));
    write_atomic(snapshot_path_, bytes);
    wal_.mark_snapshot(snapshot.index, snapshot.term);
    pending_snapshot_.reset();
    wrote = true;
  }
  if (!pending_entries_.empty()) {
    wal_.append(pending_entries_);
    pending_entries_.clear();
    wrote = true;
  }
  if (wrote) {
    wal_.sync();
  }
}

}  // namespace rme::io
