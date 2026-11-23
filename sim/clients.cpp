#include "sim/clients.hpp"

#include <algorithm>

namespace rme::sim {

Clients::Clients(ClientConfig config, std::uint64_t seed)
    : config_(config), rng_(seed), stream_(rng_.next()), clients_(config.count) {}

void Clients::collect(Time now, std::uint32_t per_mille,
                      std::vector<codec::ClientRecordBytes>& out) {
  for (Client& client : clients_) {
    if (client.pending) {
      if (now - client.pending->sent >= config_.retry_after) {
        client.pending->sent = now;
        out.push_back(client.pending->record);
        ++stats_.retries;
      }
      continue;
    }
    if (rng_.below(1000) >= per_mille) {
      continue;
    }
    if (!client.session) {
      client.nonce = next_nonce_++;
      client.pending = Pending{0, codec::encode_register(client.nonce), now};
    } else {
      const std::uint64_t seq = client.acked_seq + 1;
      client.pending = Pending{
          seq, codec::encode(engine::ClientCommand{*client.session, seq, stream_.next()}), now};
    }
    out.push_back(client.pending->record);
    ++stats_.sent;
  }
}

void Clients::on_outcome(const engine::Outcome& outcome) {
  if (outcome.result == engine::SessionResult::kRegistered) {
    const auto client = std::ranges::find_if(clients_, [&](const Client& c) {
      return !c.session && c.pending && c.nonce == outcome.seq;
    });
    if (client != clients_.end()) {
      client->session = outcome.client;
      client->acked_seq = 0;
      client->pending.reset();
      ++stats_.registered;
    }
    return;
  }
  const auto client = std::ranges::find_if(
      clients_, [&](const Client& c) { return c.session == outcome.client && c.pending; });
  if (client == clients_.end() || client->pending->seq != outcome.seq) {
    return;
  }
  if (outcome.result == engine::SessionResult::kApplied ||
      outcome.result == engine::SessionResult::kDuplicate) {
    client->acked_seq = outcome.seq;
    client->pending.reset();
    acked_.emplace_back(outcome.client, outcome.seq);
    ++stats_.acked;
  } else if (outcome.result == engine::SessionResult::kUnknownSession) {
    client->session.reset();
    client->pending.reset();
    ++stats_.expired;
  }
}

}  // namespace rme::sim
