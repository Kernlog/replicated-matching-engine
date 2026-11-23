#include <gtest/gtest.h>

#include <iostream>

#include "sim/cluster.hpp"

namespace rme::sim {
namespace {

TEST(ZzDebug, Clients) {
  SimConfig config;
  config.duration = Time(20'000);
  config.faults.crash_one_in = 0;
  const SimStats stats = Cluster(config).run();
  std::cout << "sent=" << stats.clients.sent << " retries=" << stats.clients.retries
            << " registered=" << stats.clients.registered << " acked=" << stats.clients.acked
            << " expired=" << stats.clients.expired << " proposed=" << stats.proposed << "\n";
}

}  // namespace
}  // namespace rme::sim
