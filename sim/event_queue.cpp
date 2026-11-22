#include "sim/event_queue.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "core/assert.hpp"

namespace rme::sim {

void EventQueue::schedule(Time at, Event event) {
  RME_ASSERT(at >= now_);
  events_.emplace(std::pair{at, next_seq_++}, std::move(event));
}

std::optional<Event> EventQueue::pop_until(Time end) {
  if (events_.empty() || events_.begin()->first.first > end) {
    now_ = std::max(now_, end);
    return std::nullopt;
  }
  auto node = events_.extract(events_.begin());
  now_ = node.key().first;
  return std::move(node.mapped());
}

}  // namespace rme::sim
