#include "sim/network.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "core/assert.hpp"

namespace rme::sim {

namespace {

constexpr std::uint64_t kPerMille = 1000;

}  // namespace

Network::Network(EventQueue& queue, NetworkConfig config, std::uint64_t seed)
    : queue_(&queue), config_(config), rng_(seed) {
  RME_ASSERT(config_.min_latency > Time(0) && config_.min_latency <= config_.max_latency);
}

void Network::send(consensus::Message message) {
  ++stats_.sent;
  if (rng_.chance(config_.drop, kPerMille)) {
    ++stats_.dropped;
    return;
  }
  const Time now = queue_->now();
  if (rng_.chance(config_.duplicate, kPerMille)) {
    ++stats_.duplicated;
    queue_->schedule(now + latency(), Delivery{message});
  }
  if (rng_.chance(config_.reorder, kPerMille)) {
    ++stats_.reordered;
    queue_->schedule(now + config_.min_latency, Delivery{std::move(message)});
    return;
  }
  Time at = now + latency();
  if (rng_.chance(config_.delay, kPerMille)) {
    ++stats_.delayed;
    at += config_.delay_spike;
  }
  Time& tail = tail_[{message.from, message.to}];
  tail = std::max(tail, at);
  queue_->schedule(tail, Delivery{std::move(message)});
}

Time Network::latency() {
  const std::uint64_t span = (config_.max_latency - config_.min_latency).value();
  return config_.min_latency + Time(rng_.below(span + 1));
}

}  // namespace rme::sim
