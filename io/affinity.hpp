#pragma once

#include <optional>

namespace rme::io {

// Pins the calling thread to one CPU, so its caches stay warm and the scheduler never migrates
// it. Pick CPUs on distinct physical cores; SMT siblings share the core's caches and ports.
void pin_this_thread(std::optional<int> cpu);

// Locks every current and future page in RAM, so the hot path never takes a major fault.
void lock_all_memory();

}  // namespace rme::io
