#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <variant>

#include "consensus/types.hpp"
#include "core/strong.hpp"

namespace rme::sim {

struct TimeTag {
  static constexpr bool kAdditive = true;
};

using Time = Strong<std::uint64_t, TimeTag>;

struct Tick {
  consensus::NodeId node;

  friend bool operator==(const Tick&, const Tick&) = default;
};

struct Delivery {
  consensus::Message message;

  friend bool operator==(const Delivery&, const Delivery&) = default;
};

using Event = std::variant<Tick, Delivery>;

class EventQueue {
 public:
  // Events due at the same time pop in the order they were scheduled.
  void schedule(Time at, Event event);
  void after(Time delay, Event event) { schedule(now_ + delay, std::move(event)); }

  // Pops the earliest event due at or before end and moves now() to it; otherwise moves now() to
  // end and returns nullopt.
  std::optional<Event> pop_until(Time end);

  [[nodiscard]] Time now() const noexcept { return now_; }
  [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

 private:
  std::map<std::pair<Time, std::uint64_t>, Event> events_;
  Time now_;
  std::uint64_t next_seq_ = 0;
};

}  // namespace rme::sim
