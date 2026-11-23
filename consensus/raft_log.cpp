#include "consensus/raft_log.hpp"

#include <algorithm>
#include <utility>

#include "core/assert.hpp"

namespace rme::consensus {

RaftLog::RaftLog(std::vector<Entry> stable, Index snapshot_index, Term snapshot_term)
    : entries_(std::move(stable)),
      snapshot_index_(snapshot_index),
      snapshot_term_(snapshot_term),
      stable_(last_index()) {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    RME_ASSERT(entries_[i].index == snapshot_index_ + Index(i + 1));
  }
}

Term RaftLog::last_term() const noexcept {
  return entries_.empty() ? snapshot_term_ : entries_.back().term;
}

std::optional<Term> RaftLog::term_at(Index index) const noexcept {
  if (index == snapshot_index_) {
    return snapshot_term_;
  }
  if (index < snapshot_index_ || index > last_index()) {
    return std::nullopt;
  }
  return at(index).term;
}

bool RaftLog::matches(Index index, Term term) const noexcept {
  const auto found = term_at(index);
  return found && *found == term;
}

const Entry& RaftLog::at(Index index) const noexcept {
  RME_ASSERT(index > snapshot_index_ && index <= last_index());
  return entries_[offset(index)];
}

Index RaftLog::first_index_of_term_at(Index index) const noexcept {
  const Term term = at(index).term;
  while (index > snapshot_index_ + Index(1) && at(index - Index(1)).term == term) {
    index -= Index(1);
  }
  return index;
}

std::optional<Index> RaftLog::last_index_of(Term term) const noexcept {
  for (Index index = last_index(); index > snapshot_index_; index -= Index(1)) {
    if (at(index).term == term) {
      return index;
    }
    if (at(index).term < term) {
      return std::nullopt;
    }
  }
  return snapshot_term_ == term && snapshot_index_ > Index(0) ? std::optional(snapshot_index_)
                                                              : std::nullopt;
}

Index RaftLog::append(Term term, EntryType type, std::vector<std::byte> data) {
  RME_ASSERT(term >= last_term());
  const Index index = last_index() + Index(1);
  entries_.push_back(Entry{term, index, type, std::move(data)});
  return index;
}

Index RaftLog::merge(Index prev_index, std::span<const Entry> entries) {
  RME_ASSERT(prev_index <= last_index());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const Entry& entry = entries[i];
    RME_ASSERT(entry.index == prev_index + Index(i + 1));
    if (entry.index <= snapshot_index_ || matches(entry.index, entry.term)) {
      continue;
    }
    truncate_after(entry.index - Index(1));
    entries_.insert(entries_.end(), entries.begin() + static_cast<std::ptrdiff_t>(i),
                    entries.end());
    break;
  }
  return prev_index + Index(entries.size());
}

void RaftLog::compact(Index index) {
  RME_ASSERT(index >= snapshot_index_ && index <= stable_);
  if (index == snapshot_index_) {
    return;
  }
  snapshot_term_ = at(index).term;
  entries_.erase(entries_.begin(),
                 entries_.begin() + static_cast<std::ptrdiff_t>(offset(index) + 1));
  snapshot_index_ = index;
}

void RaftLog::install(Index index, Term term) {
  if (index <= last_index() && index > snapshot_index_ && at(index).term == term) {
    entries_.erase(entries_.begin(),
                   entries_.begin() + static_cast<std::ptrdiff_t>(offset(index) + 1));
  } else {
    entries_.clear();
  }
  snapshot_index_ = index;
  snapshot_term_ = term;
  stable_ = std::max(std::min(stable_, last_index()), index);
}

std::span<const Entry> RaftLog::slice(Index from, std::size_t max) const noexcept {
  if (from > last_index() || from <= snapshot_index_) {
    return {};
  }
  const std::span<const Entry> all(entries_);
  const std::size_t begin = offset(from);
  return all.subspan(begin, std::min(max, all.size() - begin));
}

std::span<const Entry> RaftLog::unstable() const noexcept {
  return std::span<const Entry>(entries_).subspan((stable_ - snapshot_index_).value());
}

void RaftLog::mark_stable(Index index) noexcept {
  RME_ASSERT(index <= last_index());
  stable_ = std::max(stable_, index);
}

void RaftLog::truncate_after(Index index) {
  RME_ASSERT(index >= snapshot_index_ && index <= last_index());
  entries_.resize((index - snapshot_index_).value(), Entry{});
  stable_ = std::min(stable_, index);
}

}  // namespace rme::consensus
