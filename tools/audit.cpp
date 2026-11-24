#include <CLI/CLI.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "core/hash.hpp"
#include "engine/event_words.hpp"
#include "engine/replica.hpp"
#include "io/disk_storage.hpp"

namespace {

struct Applied {
  std::uint64_t index = 0;
  std::uint64_t digest = 0;
};

struct Replayed {
  std::vector<rme::consensus::Entry> log;
  // (session, seq) to every index that executed it.
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<Applied>> applied;
  std::vector<std::uint64_t> hash_at;
};

std::uint64_t events_digest(const std::vector<rme::engine::Event>& events) {
  rme::Hasher hasher;
  for (const rme::engine::Event& event : events) {
    for (const std::uint64_t word : rme::engine::to_words(event)) {
      hasher.add(word);
    }
  }
  return hasher.digest();
}

Replayed replay(const std::string& dir) {
  rme::io::DiskStorage storage(dir);
  Replayed out;
  rme::io::Durable durable = storage.take_recovered();
  if (durable.snapshot) {
    throw std::runtime_error(dir + " holds a snapshot; the audit needs the whole log");
  }
  out.log = std::move(durable.log);
  rme::engine::Replica replica;
  std::vector<rme::engine::Outcome> outcomes;
  for (const rme::consensus::Entry& entry : out.log) {
    outcomes.clear();
    replica.apply(entry.index.value(), entry.data, outcomes);
    for (const rme::engine::Outcome& outcome : outcomes) {
      if (outcome.result == rme::engine::SessionResult::kApplied) {
        out.applied[{outcome.client, outcome.seq}].push_back(
            {entry.index.value(), events_digest(outcome.events)});
      }
    }
    out.hash_at.push_back(entry.index.value() % 64 == 0 ? replica.hash() : 0);
  }
  return out;
}

}  // namespace

// Checks the WALs a chaos run left against its client history: every acked command is in a
// majority of logs at the acked index with the acked events, no command executed twice in any log,
// and replicas agree on the book wherever their logs agree.
int main(int argc, char** argv) {
  CLI::App app{"Audit a chaos run"};
  std::string history_path;
  std::vector<std::string> dirs;
  app.add_option("--history", history_path)->required();
  app.add_option("--data", dirs, "Each node's data directory")->required();
  CLI11_PARSE(app, argc, argv);

  std::vector<Replayed> nodes;
  for (const std::string& dir : dirs) {
    nodes.push_back(replay(dir));
  }
  std::uint64_t violations = 0;
  const auto fail = [&](const std::string& what) {
    if (++violations <= 20) {
      std::cout << "VIOLATION " << what << '\n';
    }
  };

  for (std::size_t n = 0; n < nodes.size(); ++n) {
    for (const auto& [key, runs] : nodes[n].applied) {
      if (runs.size() > 1) {
        fail("node " + std::to_string(n + 1) + " executed session " + std::to_string(key.first) +
             " seq " + std::to_string(key.second) + " " + std::to_string(runs.size()) + " times");
      }
    }
  }

  std::ifstream history(history_path);
  std::string line;
  std::uint64_t acked = 0;
  std::uint64_t indeterminate = 0;
  while (std::getline(history, line)) {
    std::istringstream fields(line);
    std::uint64_t worker = 0;
    std::uint64_t session = 0;
    std::uint64_t seq = 0;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::string outcome;
    std::uint64_t index = 0;
    std::uint64_t digest = 0;
    fields >> worker >> session >> seq >> start >> end >> outcome >> index >> digest;
    if (outcome != "ok") {
      ++indeterminate;
      continue;
    }
    ++acked;
    std::size_t holders = 0;
    for (const Replayed& node : nodes) {
      const auto found = node.applied.find({session, seq});
      holders += found != node.applied.end() && found->second.front().index == index &&
                         found->second.front().digest == digest
                     ? 1
                     : 0;
    }
    if (holders * 2 <= nodes.size()) {
      fail("acked session " + std::to_string(session) + " seq " + std::to_string(seq) +
           " at index " + std::to_string(index) + " is in " + std::to_string(holders) + " logs");
    }
  }

  std::uint64_t compared = 0;
  for (std::size_t a = 0; a < nodes.size(); ++a) {
    for (std::size_t b = a + 1; b < nodes.size(); ++b) {
      const std::size_t common = std::min(nodes[a].log.size(), nodes[b].log.size());
      for (std::size_t i = 0; i < common && nodes[a].log[i] == nodes[b].log[i]; ++i) {
        ++compared;
        if (nodes[a].hash_at[i] != nodes[b].hash_at[i]) {
          fail("nodes " + std::to_string(a + 1) + " and " + std::to_string(b + 1) +
               " disagree on the book at index " + std::to_string(i + 1));
          break;
        }
      }
    }
  }

  std::cout << "acked=" << acked << " indeterminate=" << indeterminate;
  for (std::size_t n = 0; n < nodes.size(); ++n) {
    std::cout << " log" << n + 1 << '=' << nodes[n].log.size();
  }
  std::cout << " hash_checks=" << compared << " violations=" << violations << '\n';
  return violations == 0 ? 0 : 1;
}
