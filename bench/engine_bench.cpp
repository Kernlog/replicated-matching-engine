#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/order_book.hpp"
#include "testing/command_stream.hpp"

namespace {

constexpr std::size_t kCommands = 1'000'000;

const std::vector<rme::engine::Command>& command_stream() {
  static const std::vector<rme::engine::Command> commands = [] {
    rme::testing::CommandStream stream(1);
    std::vector<rme::engine::Command> out;
    out.reserve(kCommands);
    for (std::size_t i = 0; i < kCommands; ++i) {
      out.push_back(stream.next());
    }
    return out;
  }();
  return commands;
}

void apply_stream(benchmark::State& state) {
  const auto& commands = command_stream();
  std::vector<rme::engine::Event> events;
  events.reserve(1024);
  for (auto _ : state) {
    rme::engine::OrderBook book;
    for (const auto& command : commands) {
      events.clear();
      book.apply(command, events);
    }
    benchmark::DoNotOptimize(book.size());
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(commands.size()));
}

}  // namespace

BENCHMARK(apply_stream)->Unit(benchmark::kMillisecond)->MinTime(2.0);

BENCHMARK_MAIN();
