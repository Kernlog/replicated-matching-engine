#include "sim/network.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/assert.hpp"

namespace rme::sim {

namespace {

constexpr std::uint64_t kPerMille = 1000;

}  // namespace

Network::Network(EventQueue& queue, NetworkConfig config, std::vector<consensus::NodeId> nodes,
                 std::uint64_t seed)
    : queue_(&queue), config_(config), nodes_(std::move(nodes)), rng_(seed) {
  RME_ASSERT(config_.min_latency > Time(0) && config_.min_latency <= config_.max_latency);
  if (config_.partitions) {
    RME_ASSERT(nodes_.size() >= 2);
    RME_ASSERT(config_.min_partition > Time(0) && config_.min_partition_gap > Time(0));
    next_change_ = queue_->now() + sample(config_.min_partition_gap, config_.max_partition_gap);
  }
}

void Network::send(consensus::Message message) {
  ++stats_.sent;
  update_partition();
  if (!connected(message.from, message.to)) {
    ++stats_.cut;
    return;
  }
  if (rng_.chance(config_.drop, kPerMille)) {
    ++stats_.dropped;
    return;
  }
  const Time now = queue_->now();
  if (rng_.chance(config_.duplicate, kPerMille)) {
    ++stats_.duplicated;
    queue_->schedule(now + sample(config_.min_latency, config_.max_latency), Delivery{message});
  }
  if (rng_.chance(config_.reorder, kPerMille)) {
    ++stats_.reordered;
    queue_->schedule(now + config_.min_latency, Delivery{std::move(message)});
    return;
  }
  Time at = now + sample(config_.min_latency, config_.max_latency);
  if (rng_.chance(config_.delay, kPerMille)) {
    ++stats_.delayed;
    at += config_.delay_spike;
  }
  Time& tail = tail_[{message.from, message.to}];
  tail = std::max(tail, at);
  queue_->schedule(tail, Delivery{std::move(message)});
}

bool Network::connected(consensus::NodeId a, consensus::NodeId b) const {
  const auto isolated = [this](consensus::NodeId id) {
    return std::ranges::find(isolated_, id) != isolated_.end();
  };
  return isolated(a) == isolated(b);
}

void Network::update_partition() {
  if (!config_.partitions) {
    return;
  }
  while (next_change_ <= queue_->now()) {
    if (isolated_.empty()) {
      split();
      next_change_ += sample(config_.min_partition, config_.max_partition);
    } else {
      isolated_.clear();
      next_change_ += sample(config_.min_partition_gap, config_.max_partition_gap);
    }
  }
}

void Network::split() {
  std::vector<consensus::NodeId> pool = nodes_;
  const std::size_t size = 1 + rng_.below(pool.size() / 2);
  for (std::size_t i = 0; i < size; ++i) {
    std::swap(pool[i], pool[i + rng_.below(pool.size() - i)]);
  }
  isolated_.assign(pool.begin(), pool.begin() + static_cast<std::ptrdiff_t>(size));
}

Time Network::sample(Time low, Time high) {
  RME_ASSERT(low <= high);
  return low + Time(rng_.below((high - low).value() + 1));
}

}  // namespace rme::sim
