#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <thread>

#include "core/spsc.hpp"

namespace {

constexpr std::uint64_t kItems = 1U << 24U;

void transfer(benchmark::State& state, std::size_t batch) {
  for (auto _ : state) {
    rme::SpscQueue<std::uint64_t> queue(1U << 14U);
    const std::jthread producer([&queue, batch] {
      std::array<std::uint64_t, 256> items{};
      std::uint64_t next = 0;
      while (next < kItems) {
        if (batch == 1) {
          queue.push(std::uint64_t{next++});
          continue;
        }
        for (std::size_t i = 0; i < batch; ++i) {
          items[i] = next + i;
        }
        next += queue.push_n(std::span(items).first(batch));
      }
    });
    std::array<std::uint64_t, 256> out{};
    std::uint64_t received = 0;
    std::uint64_t sum = 0;
    while (received < kItems) {
      const std::size_t count = queue.pop_n(std::span(out).first(batch));
      for (std::size_t i = 0; i < count; ++i) {
        sum += out[i];
      }
      received += count;
      if (count == 0) {
        rme::cpu_relax();
      }
    }
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kItems));
}

void spsc_single(benchmark::State& state) {
  transfer(state, 1);
}

void spsc_batch64(benchmark::State& state) {
  transfer(state, 64);
}

void spsc_round_trip(benchmark::State& state) {
  rme::SpscQueue<std::uint64_t> ping(1024);
  rme::SpscQueue<std::uint64_t> pong(1024);
  std::atomic<bool> stop{false};
  const std::jthread echo([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      if (auto value = ping.try_pop()) {
        pong.push(std::uint64_t{*value});
      } else {
        rme::cpu_relax();
      }
    }
  });
  std::uint64_t next = 0;
  for (auto _ : state) {
    ping.push(std::uint64_t{next++});
    while (!pong.try_pop()) {
      rme::cpu_relax();
    }
  }
  stop.store(true, std::memory_order_relaxed);
}

}  // namespace

BENCHMARK(spsc_single)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(spsc_batch64)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(spsc_round_trip)->UseRealTime();

BENCHMARK_MAIN();
