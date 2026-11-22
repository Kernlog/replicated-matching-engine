#pragma once

#include "consensus/types.hpp"
#include "core/assert.hpp"
#include "sim/event_queue.hpp"

namespace rme::sim {

// Logical time only: a node ticks once per interval, starting at its phase.
class Clock {
 public:
  Clock(EventQueue& queue, Time interval) : queue_(&queue), interval_(interval) {
    RME_ASSERT(interval > Time(0));
  }

  void start(consensus::NodeId node, Time phase) { queue_->after(phase, Tick{node}); }
  // Call when a Tick pops to schedule that node's next one.
  void ticked(const Tick& tick) { queue_->after(interval_, tick); }

  [[nodiscard]] Time now() const noexcept { return queue_->now(); }
  [[nodiscard]] Time interval() const noexcept { return interval_; }

 private:
  EventQueue* queue_;
  Time interval_;
};

}  // namespace rme::sim
