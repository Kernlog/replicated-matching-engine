#include "sim/network.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

#include "sim/event_queue.hpp"

namespace rme::sim {
namespace {

using consensus::Message;
using consensus::NodeId;
using consensus::Term;
using consensus::VoteRequest;

constexpr std::uint64_t kSends = 20000;
constexpr std::uint64_t kRate = 100;
constexpr std::uint64_t kTolerance = 15;

struct Arrival {
  std::uint64_t seq;
  Time at;
};

struct Traffic {
  std::vector<Arrival> arrivals;
  NetworkStats stats;
};

// Sends message seq from node 1 to node 2 at time seq * gap; the term carries seq.
Traffic send_stream(const NetworkConfig& config, std::uint64_t count, std::uint64_t gap) {
  EventQueue queue;
  Network network(queue, config, {NodeId(1), NodeId(2)}, 42);
  Traffic result;
  const auto drain = [&](Time end) {
    while (const auto event = queue.pop_until(end)) {
      result.arrivals.push_back({std::get<Delivery>(*event).message.term.value(), queue.now()});
    }
  };
  for (std::uint64_t seq = 0; seq < count; ++seq) {
    drain(Time(seq * gap));
    network.send(Message{NodeId(1), NodeId(2), Term(seq), VoteRequest{}});
  }
  drain(Time((count * gap) + 1000));
  result.stats = network.stats();
  return result;
}

std::uint64_t latency(const Arrival& arrival, std::uint64_t gap) {
  return arrival.at.value() - (arrival.seq * gap);
}

void expect_rate(std::uint64_t count, std::uint64_t per_mille) {
  const std::uint64_t measured = count * 1000 / kSends;
  EXPECT_GE(measured + kTolerance, per_mille) << count << " of " << kSends;
  EXPECT_LE(measured, per_mille + kTolerance) << count << " of " << kSends;
}

TEST(NetworkTest, WithoutFaultsDeliversOnceInOrderWithinLatency) {
  const Traffic result = send_stream(NetworkConfig{}, 1000, 1);
  ASSERT_EQ(result.arrivals.size(), 1000U);
  for (std::uint64_t i = 0; i < result.arrivals.size(); ++i) {
    EXPECT_EQ(result.arrivals[i].seq, i);
    EXPECT_GE(latency(result.arrivals[i], 1), 1U);
    EXPECT_LE(latency(result.arrivals[i], 1), 10U);
  }
}

TEST(NetworkTest, DropsAtTheConfiguredRate) {
  NetworkConfig config;
  config.drop = kRate;
  const Traffic result = send_stream(config, kSends, 1);
  const std::uint64_t lost = kSends - result.arrivals.size();
  EXPECT_EQ(lost, result.stats.dropped);
  expect_rate(lost, kRate);
}

TEST(NetworkTest, DuplicatesAtTheConfiguredRate) {
  NetworkConfig config;
  config.duplicate = kRate;
  const Traffic result = send_stream(config, kSends, 1);
  std::map<std::uint64_t, int> copies;
  for (const Arrival& arrival : result.arrivals) {
    ++copies[arrival.seq];
  }
  ASSERT_EQ(copies.size(), kSends);
  const auto twice = static_cast<std::uint64_t>(
      std::ranges::count_if(copies, [](const auto& entry) { return entry.second == 2; }));
  EXPECT_EQ(twice + kSends, result.arrivals.size());
  EXPECT_EQ(twice, result.stats.duplicated);
  expect_rate(twice, kRate);
}

TEST(NetworkTest, DelaysAtTheConfiguredRateBySpike) {
  NetworkConfig config;
  config.delay = kRate;
  constexpr std::uint64_t kGap = 100;
  const Traffic result = send_stream(config, kSends, kGap);
  ASSERT_EQ(result.arrivals.size(), kSends);
  std::uint64_t late = 0;
  for (const Arrival& arrival : result.arrivals) {
    const std::uint64_t took = latency(arrival, kGap);
    if (took > 10) {
      ++late;
      EXPECT_GE(took, 51U);
      EXPECT_LE(took, 60U);
    }
  }
  EXPECT_EQ(late, result.stats.delayed);
  expect_rate(late, kRate);
}

TEST(NetworkTest, ReordersAtTheConfiguredRate) {
  NetworkConfig config;
  config.min_latency = Time(5);
  config.max_latency = Time(20);
  config.reorder = kRate;
  const Traffic result = send_stream(config, kSends, 1);
  ASSERT_EQ(result.arrivals.size(), kSends);
  std::uint64_t overtakers = 0;
  std::uint64_t later_min = UINT64_MAX;
  for (const Arrival& arrival : result.arrivals | std::views::reverse) {
    if (arrival.seq > later_min) {
      ++overtakers;
    }
    later_min = std::min(later_min, arrival.seq);
  }
  EXPECT_LE(overtakers, result.stats.reordered);
  EXPECT_GE(overtakers * 10, result.stats.reordered * 9);
  expect_rate(overtakers, kRate);
}

TEST(NetworkTest, PartitionsBlockForASampledDurationThenHeal) {
  NetworkConfig config;
  config.min_latency = Time(1);
  config.max_latency = Time(1);
  config.partitions = true;
  config.min_partition_gap = Time(50);
  config.max_partition_gap = Time(100);
  config.min_partition = Time(30);
  config.max_partition = Time(60);
  const Traffic result = send_stream(config, kSends, 1);
  std::vector<bool> delivered(kSends, false);
  for (const Arrival& arrival : result.arrivals) {
    delivered[arrival.seq] = true;
  }
  std::vector<std::pair<bool, std::uint64_t>> runs;
  for (const bool open : delivered) {
    if (runs.empty() || runs.back().first != open) {
      runs.emplace_back(open, 0);
    }
    ++runs.back().second;
  }
  ASSERT_GE(runs.size(), 3U);
  std::uint64_t blocked = 0;
  for (std::size_t i = 1; i + 1 < runs.size(); ++i) {
    const auto [open, length] = runs[i];
    EXPECT_GE(length, open ? 50U : 30U);
    EXPECT_LE(length, open ? 100U : 60U);
    blocked += open ? 0 : 1;
  }
  EXPECT_GE(blocked, kSends / 200);
  EXPECT_EQ(result.stats.cut, kSends - result.arrivals.size());
}

TEST(NetworkTest, PartitionCutsOffAMinority) {
  NetworkConfig config;
  config.partitions = true;
  config.min_partition_gap = Time(1);
  config.max_partition_gap = Time(5);
  config.min_partition = Time(1);
  config.max_partition = Time(5);
  const std::vector<NodeId> nodes = {NodeId(1), NodeId(2), NodeId(3), NodeId(4), NodeId(5)};
  EventQueue queue;
  Network network(queue, config, nodes, 7);
  std::map<std::size_t, int> minorities;
  for (std::uint64_t t = 0; t < 1000; ++t) {
    while (queue.pop_until(Time(t))) {
    }
    network.send(Message{NodeId(1), NodeId(2), Term(t), VoteRequest{}});
    const auto with_one = static_cast<std::size_t>(
        std::ranges::count_if(nodes, [&](NodeId id) { return network.connected(NodeId(1), id); }));
    ++minorities[std::min(with_one, nodes.size() - with_one)];
  }
  EXPECT_GT(minorities[0], 0);
  EXPECT_GT(minorities[1], 0);
  EXPECT_GT(minorities[2], 0);
  EXPECT_EQ(minorities.size(), 3U);
}

}  // namespace
}  // namespace rme::sim
