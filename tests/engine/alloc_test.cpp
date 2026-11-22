#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/order_book.hpp"
#include "testing/command_stream.hpp"

namespace {

std::atomic<std::size_t> allocations{0};

}  // namespace

void* operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, [[maybe_unused]] std::size_t size) noexcept {
  std::free(memory);
}

namespace rme::engine {
namespace {

TEST(AllocationTest, SteadyStateApplyDoesNotAllocate) {
  testing::CommandStream stream(11);
  std::vector<Command> commands;
  commands.reserve(200'000);
  for (int i = 0; i < 200'000; ++i) {
    commands.push_back(stream.next());
  }
  OrderBook book;
  std::vector<Event> events;
  events.reserve(4096);

  const std::size_t before = allocations.load();
  for (const Command& command : commands) {
    events.clear();
    book.apply(command, events);
  }
  EXPECT_EQ(allocations.load() - before, 0U);
  EXPECT_GT(book.size(), 0U);
}

}  // namespace
}  // namespace rme::engine
