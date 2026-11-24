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
constexpr std::size_t kReadBytes = std::size_t{1} << 20U;
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

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  const log::File file(path, O_RDONLY);
  ::posix_fadvise(file.fd(), 0, 0, POSIX_FADV_SEQUENTIAL);
  std::vector<std::byte> data(std::filesystem::file_size(path));
  std::size_t at = 0;
  while (at < data.size()) {
    const ssize_t n = ::pread(file.fd(), &data[at], std::min(kReadBytes, data.size() - at),
                              static_cast<off_t>(at));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      throw_errno("read " + path.string());
    }
    at += static_cast<std::size_t>(n);
  }
  return data;
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
  for (const std::filesystem::path& path : paths) {
    offset_ = replay_segment(path);
  }
  if (!paths.empty()) {
    current_.emplace(paths.back(), O_RDWR);
    sequence_ = std::stoull(paths.back().stem().string());
  }
}

std::uint64_t Wal::replay_segment(const std::filesystem::path& path) {
  const std::vector<std::byte> data = read_file(path);
  std::uint64_t offset = 0;
  while (offset + sizeof(RecordHeader) <= data.size()) {
    RecordHeader header;
    std::memcpy(&header, &data[offset], sizeof header);
    if (header.length == 0 && header.crc == 0 && header.index == 0) {
      break;
    }
    const bool known = header.kind == RecordKind::kEntry || header.kind == RecordKind::kSnapshot;
    if (!known || offset + record_bytes(header.length) > data.size()) {
      corrupt(path, offset);
    }
    const auto body = std::span(data).subspan(offset + sizeof header, header.length);
    if (record_crc(header, body) != header.crc) {
      corrupt(path, offset);
    }
    apply(header, body);
    offset += record_bytes(header.length);
  }
  return offset;
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

void Wal::flush() {
  if (staged_bytes_ == 0) {
    return;
  }
  pwritev_all(current_->fd(), iov_, static_cast<off_t>(offset_));
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
  preallocate(current_->fd(), options_.segment_bytes);
  sync_file(current_->fd());
  sync_dir(dir_);
  offset_ = 0;
}

void Wal::sync() {
  flush();
  for (const log::File& file : unsynced_) {
    sync_data(file.fd());
  }
  unsynced_.clear();
  sync_data(current_->fd());
  ++stats_.syncs;
}

}  // namespace rme::io
