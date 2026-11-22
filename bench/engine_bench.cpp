#include <benchmark/benchmark.h>
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <vector>

#include "codec/crc32c.hpp"
#include "codec/record.hpp"
#include "engine/order_book.hpp"
#include "log/command_log.hpp"
#include "testing/command_stream.hpp"

namespace {

std::size_t g_allocs = 0;
std::size_t g_alloc_bytes = 0;

void* counted(std::size_t size, std::size_t alignment) {
  ++g_allocs;
  g_alloc_bytes += size;
  const std::size_t rounded = (size + alignment - 1) / alignment * alignment;
  if (void* p = std::aligned_alloc(alignment, rounded == 0 ? alignment : rounded)) {
    return p;
  }
  throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t size) {
  return counted(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t align) {
  return counted(size, static_cast<std::size_t>(align));
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t /*size*/) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t /*align*/) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t /*size*/, std::align_val_t /*align*/) noexcept {
  std::free(p);
}

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

  const std::size_t bytes_before = g_alloc_bytes;
  rme::engine::OrderBook book;
  state.counters["book_bytes"] = static_cast<double>(g_alloc_bytes - bytes_before);
  const std::size_t allocs_before = g_allocs;
  for (const auto& command : commands) {
    events.clear();
    book.apply(command, events);
  }
  state.counters["allocs_per_kcmd"] =
      static_cast<double>(g_allocs - allocs_before) * 1000.0 / static_cast<double>(commands.size());
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const long max_rss_kib = usage.ru_maxrss;  // NOLINT(*-union-access): glibc wraps it in a union
  state.counters["peak_rss_mb"] = static_cast<double>(max_rss_kib) / 1024.0;
}

void apply_latency(benchmark::State& state) {
  using Clock = std::chrono::steady_clock;
  const auto& commands = command_stream();
  std::vector<rme::engine::Event> events;
  events.reserve(1024);
  std::vector<std::int64_t> samples(commands.size());
  for (auto _ : state) {
    rme::engine::OrderBook book;
    for (std::size_t i = 0; i < commands.size(); ++i) {
      events.clear();
      const auto start = Clock::now();
      book.apply(commands[i], events);
      samples[i] =
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    }
  }
  std::ranges::sort(samples);
  const auto at = [&](double quantile) {
    const auto rank = static_cast<double>(samples.size() - 1) * quantile;
    return static_cast<double>(samples[static_cast<std::size_t>(rank)]);
  };
  state.counters["p50_ns"] = at(0.50);
  state.counters["p99_ns"] = at(0.99);
  state.counters["p999_ns"] = at(0.999);
  state.counters["max_ns"] = at(1.0);
}

void encode_record(benchmark::State& state) {
  const auto& commands = command_stream();
  std::size_t i = 0;
  for (auto _ : state) {
    auto bytes = rme::codec::encode(commands[i]);
    benchmark::DoNotOptimize(bytes);
    i = i + 1 == commands.size() ? 0 : i + 1;
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

void decode_record(benchmark::State& state) {
  const auto& commands = command_stream();
  std::vector<rme::codec::RecordBytes> records;
  records.reserve(4096);
  for (std::size_t i = 0; i < 4096; ++i) {
    records.push_back(rme::codec::encode(commands[i]));
  }
  std::size_t i = 0;
  for (auto _ : state) {
    auto command = rme::codec::decode(records[i]);
    benchmark::DoNotOptimize(command);
    i = (i + 1) & 4095U;
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

void crc32c_4k(benchmark::State& state) {
  std::vector<std::byte> buffer(4096);
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    buffer[i] = static_cast<std::byte>(i * 131U);
  }
  for (auto _ : state) {
    benchmark::DoNotOptimize(rme::codec::crc32c(buffer));
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(buffer.size()));
}

void log_replay(benchmark::State& state) {
  const auto path = std::filesystem::temp_directory_path() / "rme_bench_replay.log";
  std::filesystem::remove(path);
  {
    rme::log::LogWriter writer(path);
    for (const auto& command : command_stream()) {
      writer.append(command);
    }
    writer.flush();
  }
  std::int64_t replayed = 0;
  for (auto _ : state) {
    rme::log::LogReader reader(path);
    while (const auto command = reader.next()) {
      ++replayed;
    }
  }
  state.SetItemsProcessed(replayed);
  std::filesystem::remove(path);
}

}  // namespace

BENCHMARK(apply_stream)->Unit(benchmark::kMillisecond)->MinTime(2.0);
BENCHMARK(apply_latency)->Unit(benchmark::kMillisecond)->Iterations(3);
BENCHMARK(encode_record);
BENCHMARK(decode_record);
BENCHMARK(crc32c_4k);
BENCHMARK(log_replay)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
