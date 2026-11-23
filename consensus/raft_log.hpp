#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "consensus/types.hpp"

namespace rme::consensus {

// Entries after a snapshot point (index, term); everything at or below it is compacted away.
class RaftLog {
 public:
  RaftLog() = default;
  explicit RaftLog(std::vector<Entry> stable, Index snapshot_index = Index(0),
                   Term snapshot_term = Term(0));

  [[nodiscard]] Index snapshot_index() const noexcept { return snapshot_index_; }
  [[nodiscard]] Term snapshot_term() const noexcept { return snapshot_term_; }
  [[nodiscard]] Index last_index() const noexcept {
    return snapshot_index_ + Index(entries_.size());
  }
  [[nodiscard]] Term last_term() const noexcept;
  [[nodiscard]] std::optional<Term> term_at(Index index) const noexcept;
  [[nodiscard]] bool matches(Index index, Term term) const noexcept;
  [[nodiscard]] const Entry& at(Index index) const noexcept;
  [[nodiscard]] Index first_index_of_term_at(Index index) const noexcept;
  [[nodiscard]] std::optional<Index> last_index_of(Term term) const noexcept;

  Index append(Term term, EntryType type, std::vector<std::byte> data);
  // Entries must follow prev_index contiguously; prev_index must already match or be compacted.
  // Returns the index of the last entry covered by the request.
  Index merge(Index prev_index, std::span<const Entry> entries);
  // Drops entries up to index, which must be stable.
  void compact(Index index);
  // Moves the snapshot point to (index, term), keeping later entries only if the log agrees there.
  void install(Index index, Term term);

  [[nodiscard]] std::span<const Entry> slice(Index from, std::size_t max) const noexcept;
  [[nodiscard]] std::span<const Entry> unstable() const noexcept;
  void mark_stable(Index index) noexcept;
  [[nodiscard]] Index stable_index() const noexcept { return stable_; }

 private:
  void truncate_after(Index index);
  [[nodiscard]] std::size_t offset(Index index) const noexcept {
    return (index - snapshot_index_).value() - 1;
  }

  std::vector<Entry> entries_;
  Index snapshot_index_;
  Term snapshot_term_;
  Index stable_;
};

}  // namespace rme::consensus
