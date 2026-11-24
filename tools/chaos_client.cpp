#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/hash.hpp"
#include "engine/event_words.hpp"
#include "gateway/client.hpp"
#include "testing/command_stream.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t events_digest(const std::vector<rme::engine::Event>& events) {
  rme::Hasher hasher;
  for (const rme::engine::Event& event : events) {
    for (const std::uint64_t word : rme::engine::to_words(event)) {
      hasher.add(word);
    }
  }
  return hasher.digest();
}

class History {
 public:
  explicit History(const std::string& path) : out_(path) {}

  // One line per operation: worker, session, seq, invoke and complete time, outcome, log index
  // and a digest of the events the ack carried.
  void record(std::uint32_t worker, std::uint64_t session, std::uint64_t seq,
              Clock::time_point start, Clock::time_point end, const std::string& outcome,
              std::uint64_t index, std::uint64_t digest) {
    const std::scoped_lock lock(mutex_);
    out_ << worker << '\t' << session << '\t' << seq << '\t' << since(start) << '\t' << since(end)
         << '\t' << outcome << '\t' << index << '\t' << digest << '\n';
  }

  void flush() {
    const std::scoped_lock lock(mutex_);
    out_.flush();
  }

 private:
  [[nodiscard]] std::int64_t since(Clock::time_point at) const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(at - epoch_).count();
  }

  std::mutex mutex_;
  std::ofstream out_;
  Clock::time_point epoch_ = Clock::now();
};

void work(std::uint32_t worker, const std::vector<rme::gateway::Endpoint>& members,
          Clock::time_point stop, History& history) {
  rme::gateway::ClientOptions options;
  options.members = members;
  options.first = worker % members.size();
  options.nonce = 1000 + worker;
  options.give_up_after = std::chrono::seconds(20);
  rme::gateway::Client client(options);
  try {
    client.open();
  } catch (const std::exception& error) {
    std::cerr << "worker " << worker << ": " << error.what() << '\n';
    return;
  }
  rme::testing::CommandStream stream(worker + 1);
  std::uint64_t seq = 0;
  while (Clock::now() < stop) {
    const auto start = Clock::now();
    ++seq;
    try {
      const rme::gateway::Ack ack = client.submit(stream.next());
      history.record(worker, client.session(), seq, start, Clock::now(), "ok", ack.index,
                     events_digest(ack.events));
    } catch (const std::exception& error) {
      history.record(worker, client.session(), seq, start, Clock::now(), "info", 0, 0);
      return;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Chaos workload: retrying clients that record a history"};
  std::vector<std::string> members;
  std::uint32_t workers = 8;
  std::uint32_t seconds = 60;
  std::string out = "history.tsv";
  app.add_option("--member", members, "host:port of every gateway in id order")->required();
  app.add_option("--workers", workers, "Concurrent clients");
  app.add_option("--seconds", seconds, "Run time");
  app.add_option("--out", out, "History file");
  CLI11_PARSE(app, argc, argv);

  std::vector<rme::gateway::Endpoint> endpoints;
  for (const std::string& member : members) {
    const std::size_t colon = member.rfind(':');
    endpoints.push_back({member.substr(0, colon),
                         static_cast<std::uint16_t>(std::stoul(member.substr(colon + 1)))});
  }
  History history(out);
  const auto stop = Clock::now() + std::chrono::seconds(seconds);
  {
    std::vector<std::jthread> threads;
    for (std::uint32_t worker = 0; worker < workers; ++worker) {
      threads.emplace_back(work, worker, std::cref(endpoints), stop, std::ref(history));
    }
  }
  history.flush();
  return 0;
}
