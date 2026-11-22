#include "consensus/raft_log.hpp"

#include <algorithm>
#include <utility>

#include "core/assert.hpp"

namespace rme::consensus {

RaftLog::RaftLog(std::vector<Entry> stable) : entries_(std::move(stable)), stable_(last_index()) {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    RME_ASSERT(entries_[i].index == Index(i + 1));
  }
}

Term RaftLog::last_term() const noexcept {
  return entries_.empty() ? Term(0) : entries_.back().term;
}

std::optional<Term> RaftLog::term_at(Index index) const noexcept {
  if (index == Index(0)) {
    return Term(0);
  }
  if (index > last_index()) {
    return std::nullopt;
  }
  return at(index).term;
}

bool RaftLog::matches(Index index, Term term) const noexcept {
  const auto found = term_at(index);
  return found && *found == term;
}

const Entry& RaftLog::at(Index index) const noexcept {
  RME_ASSERT(index > Index(0) && index <= last_index());
  return entries_[index.value() - 1];
}

Index RaftLog::first_index_of_term_at(Index index) const noexcept {
  const Term term = at(index).term;
  while (index > Index(1) && at(index - Index(1)).term == term) {
    index -= Index(1);
  }
  return index;
}

std::optional<Index> RaftLog::last_index_of(Term term) const noexcept {
  for (Index index = last_index(); index > Index(0); index -= Index(1)) {
    if (at(index).term == term) {
      return index;
    }
    if (at(index).term < term) {
      break;
    }
  }
  return std::nullopt;
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
    if (matches(entry.index, entry.term)) {
      continue;
    }
    truncate_after(entry.index - Index(1));
    entries_.insert(entries_.end(), entries.begin() + static_cast<std::ptrdiff_t>(i),
                    entries.end());
    break;
  }
  return prev_index + Index(entries.size());
}

std::span<const Entry> RaftLog::slice(Index from, std::size_t max) const noexcept {
  if (from > last_index() || from == Index(0)) {
    return {};
  }
  const std::span<const Entry> all(entries_);
  const std::size_t begin = from.value() - 1;
  return all.subspan(begin, std::min(max, all.size() - begin));
}

std::span<const Entry> RaftLog::unstable() const noexcept {
  return std::span<const Entry>(entries_).subspan(stable_.value());
}

void RaftLog::mark_stable(Index index) noexcept {
  RME_ASSERT(index <= last_index());
  stable_ = std::max(stable_, index);
}

void RaftLog::truncate_after(Index index) {
  RME_ASSERT(index <= last_index());
  entries_.resize(index.value(), Entry{});
  stable_ = std::min(stable_, index);
}

}  // namespace rme::consensus
