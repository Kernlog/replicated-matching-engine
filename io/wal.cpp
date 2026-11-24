#include "io/wal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include "codec/crc32c.hpp"
#include "core/assert.hpp"
#include "io/frame.hpp"
#include "io/fs.hpp"

namespace rme::io {

namespace {

constexpr std::size_t kMaxStaged = 512;
constexpr std::array<std::byte, 8> kZeros{};
constexpr std::string_view kSuffix = ".wal";

std::uint32_t record_crc(RecordHeader header, std::span<const std::byte> body) noexcept {
  header.crc = 0;
  return codec::crc32c(body, codec::crc32c(std::as_bytes(std::span(&header, 1))));
}

RecordHeader make_header(RecordKind kind, consensus::Term term, consensus::Index index,
                         consensus::EntryType type, std::size_t length) noexcept {
  RecordHeader header;
  header.length = static_cast<std::uint32_t>(length);
  header.term = term.value();
  header.index = index.value();
  header.kind = kind;
  header.entry_type = type;
  return header;
}

constexpr std::uint64_t record_bytes(std::size_t length) noexcept {
  return sizeof(RecordHeader) + padded(length);
}

constexpr std::size_t kSector = 512;

bool all_zero(std::span<const std::byte> bytes) noexcept {
  return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; });
}

// True when some sector the record overlaps still holds only zeros over the record's bytes.
bool has_unwritten_sector(std::span<const std::byte> record, std::uint64_t offset) noexcept {
  std::uint64_t at = 0;
  while (at < record.size()) {
    const std::uint64_t sector_end = ((offset + at) / kSector + 1) * kSector - offset;
    const std::uint64_t until = std::min<std::uint64_t>(sector_end, record.size());
    if (all_zero(record.subspan(at, until - at))) {
      return true;
    }
    at = until;
  }
  return false;
}

// A segment ends early only when the next record did not fit, so a next segment whose first
// record would have fit means this one lost its tail.
bool next_would_fit(const std::filesystem::path& next, std::uint64_t space) {
  const log::File file(next, O_RDONLY);
  RecordHeader header;
  if (::pread(file.fd(), &header, sizeof header, 0) != static_cast<ssize_t>(sizeof header)) {
    return false;
  }
  return !all_zero(std::as_bytes(std::span(&header, 1))) && record_bytes(header.length) <= space;
}

void zero_from(const std::filesystem::path& path, std::uint64_t from, std::uint64_t to) {
  const log::File file(path, O_RDWR);
  const std::vector<std::byte> zeros(std::min<std::uint64_t>(to - from, std::uint64_t{1} << 20U));
  while (from < to) {
    std::array<iovec, 1> iov{{{const_cast<std::byte*>(zeros.data()),  // NOLINT(*-const-cast)
                               std::min<std::size_t>(zeros.size(), to - from)}}};
    const std::size_t bytes = iov[0].iov_len;
    pwritev_all(file.fd(), iov, static_cast<off_t>(from));
    from += bytes;
  }
  sync_data(file.fd());
}

[[noreturn]] void corrupt(const std::filesystem::path& path, std::uint64_t offset) {
  throw std::runtime_error("corrupt wal record in " + path.string() + " at " +
                           std::to_string(offset));
}

}  // namespace

std::filesystem::path segment_path(const std::filesystem::path& dir, std::uint64_t sequence) {
  std::string name = std::to_string(sequence);
  name.insert(0, 20 - std::min<std::size_t>(20, name.size()), '0');
  return dir / (name + std::string(kSuffix));
}

Wal::Wal(std::filesystem::path dir, WalOptions options) : dir_(std::move(dir)), options_(options) {
  headers_.reserve(kMaxStaged);
  iov_.reserve(3 * kMaxStaged);
  std::filesystem::create_directories(dir_);
  replay();
  if (!current_) {
    roll();
  }
}

void Wal::replay() {
  std::vector<std::filesystem::path> paths;
  for (const auto& item : std::filesystem::directory_iterator(dir_)) {
    if (item.path().extension() == kSuffix) {
      paths.push_back(item.path());
    }
  }
  std::ranges::sort(paths);
  for (std::size_t i = 0; i < paths.size(); ++i) {
    const std::vector<std::byte> data = read_file(paths[i]);
    const auto [end, torn] = replay_segment(paths[i], data);
    const bool rest_written = !all_zero(std::span(data).subspan(end));
    const bool next_fits = i + 1 < paths.size() && next_would_fit(paths[i + 1], data.size() - end);
    if (torn || rest_written || next_fits) {
      // Everything after a torn write is unsynced, so it is dropped and zeroed so that later
      // appends cannot run into stale bytes.
      zero_from(paths[i], end, data.size());
      for (std::size_t j = i + 1; j < paths.size(); ++j) {
        std::filesystem::remove(paths[j]);
      }
      sync_dir(dir_);
      truncated_ += data.size() - end;
      paths.resize(i + 1);
    }
    offset_ = end;
  }
  if (!paths.empty()) {
    current_.emplace(paths.back(), O_RDWR);
    sequence_ = std::stoull(paths.back().stem().string());
    if (std::filesystem::file_size(paths.back()) < options_.segment_bytes) {
      preallocate(current_->fd(), options_.segment_bytes);
      sync_file(current_->fd());
    }
  }
}

// Stops at the first record that is not intact. A record with an unwritten (zero) sector is a torn
// write and ends the log there; anything else that fails its CRC is corruption and fails stop.
Wal::Replayed Wal::replay_segment(const std::filesystem::path& path,
                                  std::span<const std::byte> data) {
  std::uint64_t offset = 0;
  while (offset + sizeof(RecordHeader) <= data.size()) {
    const auto head = data.subspan(offset, sizeof(RecordHeader));
    if (all_zero(head)) {
      return {.end = offset, .torn = false};
    }
    RecordHeader header;
    std::memcpy(&header, head.data(), sizeof header);
    const bool known = header.kind == RecordKind::kEntry || header.kind == RecordKind::kSnapshot;
    const std::uint64_t size = std::min(record_bytes(header.length), data.size() - offset);
    const auto record = data.subspan(offset, size);
    const bool intact =
        known && size == record_bytes(header.length) &&
        record_crc(header, record.subspan(sizeof header, header.length)) == header.crc;
    if (!intact) {
      if (has_unwritten_sector(record, offset)) {
        return {.end = offset, .torn = true};
      }
      corrupt(path, offset);
    }
    apply(header, record.subspan(sizeof header, header.length));
    offset += size;
  }
  return {.end = offset, .torn = false};
}

void Wal::apply(const RecordHeader& header, std::span<const std::byte> body) {
  const consensus::Index index(header.index);
  const consensus::Term term(header.term);
  const consensus::Index first =
      entries_.empty() ? mark_index_ + consensus::Index(1) : entries_.front().index;
  if (header.kind == RecordKind::kSnapshot) {
    const bool held = !entries_.empty() && first <= index && index <= entries_.back().index &&
                      entries_[(index - first).value()].term == term;
    if (held) {
      entries_.erase(entries_.begin(),
                     entries_.begin() + static_cast<std::ptrdiff_t>((index - first).value() + 1));
    } else {
      entries_.clear();
    }
    mark_index_ = index;
    mark_term_ = term;
    return;
  }
  const bool started = !entries_.empty() || mark_index_ != consensus::Index(0);
  if (started) {
    const consensus::Index last = entries_.empty() ? mark_index_ : entries_.back().index;
    if (index <= mark_index_ || index > last + consensus::Index(1)) {
      throw std::runtime_error("wal entry " + std::to_string(index.value()) + " after " +
                               std::to_string(last.value()));
    }
    entries_.resize(index < first ? 0 : (index - first).value());
  }
  entries_.push_back(consensus::Entry{
      .term = term, .index = index, .type = header.entry_type, .data = {body.begin(), body.end()}});
}

void Wal::append(std::span<const consensus::Entry> entries) {
  for (const consensus::Entry& entry : entries) {
    const std::uint64_t bytes = record_bytes(entry.data.size());
    RME_ASSERT(bytes <= options_.segment_bytes);
    if (offset_ + staged_bytes_ + bytes > options_.segment_bytes) {
      flush();
      roll();
    }
    stage(make_header(RecordKind::kEntry, entry.term, entry.index, entry.type, entry.data.size()),
          entry.data);
  }
  flush();
}

void Wal::mark_snapshot(consensus::Index index, consensus::Term term) {
  if (offset_ + sizeof(RecordHeader) > options_.segment_bytes) {
    roll();
  }
  stage(make_header(RecordKind::kSnapshot, term, index, consensus::EntryType::kNoop, 0), {});
  flush();
}

void Wal::stage(const RecordHeader& header, std::span<const std::byte> body) {
  if (headers_.size() == kMaxStaged) {
    flush();
  }
  RecordHeader& staged = headers_.emplace_back(header);
  staged.crc = record_crc(staged, body);
  iov_.push_back({&staged, sizeof staged});
  if (!body.empty()) {
    iov_.push_back({const_cast<std::byte*>(body.data()), body.size()});  // NOLINT(*-const-cast)
  }
  if (const std::size_t pad = padded(body.size()) - body.size(); pad != 0) {
    iov_.push_back({const_cast<std::byte*>(kZeros.data()), pad});  // NOLINT(*-const-cast)
  }
  staged_bytes_ += record_bytes(body.size());
}

log::File& Wal::current() noexcept {
  RME_ASSERT(current_.has_value());
  return *current_;  // NOLINT(bugprone-unchecked-optional-access)
}

void Wal::flush() {
  if (staged_bytes_ == 0) {
    return;
  }
  pwritev_all(current().fd(), iov_, static_cast<off_t>(offset_));
  offset_ += staged_bytes_;
  ++stats_.writes;
  stats_.bytes += staged_bytes_;
  staged_bytes_ = 0;
  headers_.clear();
  iov_.clear();
}

void Wal::roll() {
  if (current_) {
    unsynced_.push_back(std::move(*current_));
  }
  ++sequence_;
  const std::filesystem::path path = segment_path(dir_, sequence_);
  current_.emplace(path, O_RDWR | O_CREAT | O_EXCL);
  preallocate(current().fd(), options_.segment_bytes);
  sync_file(current().fd());
  sync_dir(dir_);
  offset_ = 0;
}

void Wal::sync() {
  flush();
  for (const log::File& file : unsynced_) {
    sync_data(file.fd());
  }
  unsynced_.clear();
  sync_data(current().fd());
  ++stats_.syncs;
}

}  // namespace rme::io
