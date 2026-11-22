#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "consensus/types.hpp"

namespace rme::consensus {

class RaftLog {
 public:
  RaftLog() = default;
  explicit RaftLog(std::vector<Entry> stable);

  [[nodiscard]] Index last_index() const noexcept { return Index(entries_.size()); }
  [[nodiscard]] Term last_term() const noexcept;
  [[nodiscard]] std::optional<Term> term_at(Index index) const noexcept;
  [[nodiscard]] bool matches(Index index, Term term) const noexcept;
  [[nodiscard]] const Entry& at(Index index) const noexcept;

  Index append(Term term, EntryType type, std::vector<std::byte> data);
  // Entries must follow prev_index contiguously; prev_index must already match.
  // Returns the index of the last entry covered by the request.
  Index merge(Index prev_index, std::span<const Entry> entries);

  [[nodiscard]] std::span<const Entry> slice(Index from, std::size_t max) const noexcept;
  [[nodiscard]] std::span<const Entry> unstable() const noexcept;
  void mark_stable(Index index) noexcept;
  [[nodiscard]] Index stable_index() const noexcept { return stable_; }

 private:
  void truncate_after(Index index);

  std::vector<Entry> entries_;
  Index stable_;
};

}  // namespace rme::consensus
