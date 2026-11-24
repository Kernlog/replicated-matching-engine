#include <hdr/hdr_histogram.h>

#include <CLI/CLI.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gateway/client.hpp"
#include "gateway/protocol.hpp"
#include "io/frame.hpp"
#include "testing/command_stream.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t ns(Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}

struct HistogramDeleter {
  void operator()(hdr_histogram* histogram) const noexcept { hdr_close(histogram); }
};
using Histogram = std::unique_ptr<hdr_histogram, HistogramDeleter>;

Histogram make_histogram() {
  hdr_histogram* raw = nullptr;
  // 1 us to 60 s at 3 significant digits, in nanoseconds.
  if (hdr_init(1'000, 60'000'000'000, 3, &raw) != 0) {
    throw std::runtime_error("hdr_init");
  }
  return Histogram(raw);
}

// One pipelined session on the leader's gateway. The sender stamps each seq with the time it was
// meant to go out; the reader records ack time minus that, so a stall shows up as latency for
// every command scheduled behind it instead of hiding (no coordinated omission).
class Stream {
 public:
  Stream(asio::io_context& io, const rme::gateway::Endpoint& leader, std::uint64_t nonce,
         std::size_t capacity)
      : socket_(io), intended_(capacity + 1), histogram_(make_histogram()), stream_(nonce) {
    asio::ip::tcp::resolver resolver(io);
    asio::connect(socket_, resolver.resolve(leader.host, std::to_string(leader.port)));
    socket_.set_option(asio::ip::tcp::no_delay(true));
    std::vector<std::byte> frame;
    rme::gateway::encode_submit(rme::codec::encode_register(nonce), frame);
    asio::write(socket_, asio::buffer(frame));
    const rme::gateway::Ack ack = next_ack().value_or(rme::gateway::Ack{});
    if (ack.result != rme::engine::SessionResult::kRegistered) {
      throw std::runtime_error("registration was not acked; is this node the leader?");
    }
    session_ = ack.client;
  }

  void send(Clock::time_point intended, bool record) {
    const std::uint64_t seq = ++sent_;
    intended_[seq].store(record ? ns(intended) : -1, std::memory_order_release);
    frame_.clear();
    rme::gateway::encode_submit(rme::codec::encode(rme::engine::ClientCommand{
                                    .client = session_, .seq = seq, .command = stream_.next()}),
                                frame_);
    asio::write(socket_, asio::buffer(frame_));
  }

  void read_until(std::uint64_t count, Clock::time_point deadline) {
    while (acked_ < count && Clock::now() < deadline) {
      const std::optional<rme::gateway::Ack> ack = next_ack();
      if (!ack) {
        return;
      }
      ++acked_;
      applied_ += ack->result == rme::engine::SessionResult::kApplied ? 1 : 0;
      const std::int64_t intended = intended_[ack->seq].load(std::memory_order_acquire);
      if (intended >= 0) {
        hdr_record_value(histogram_.get(), ns(Clock::now()) - intended);
      }
    }
  }

  [[nodiscard]] std::uint64_t acked() const noexcept { return acked_; }
  [[nodiscard]] std::uint64_t applied() const noexcept { return applied_; }
  [[nodiscard]] hdr_histogram* histogram() const noexcept { return histogram_.get(); }
  void close() {
    asio::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
  }

 private:
  std::optional<rme::gateway::Ack> next_ack() {
    for (;;) {
      if (const std::optional<rme::io::Frame> frame = decoder_.next()) {
        return rme::gateway::decode_ack(frame->body);
      }
      const std::span<std::byte> space = decoder_.writable();
      asio::error_code error;
      const std::size_t read = socket_.read_some(asio::buffer(space.data(), space.size()), error);
      if (error) {
        return std::nullopt;
      }
      decoder_.commit(read);
    }
  }

  asio::ip::tcp::socket socket_;
  std::vector<std::atomic<std::int64_t>> intended_;
  Histogram histogram_;
  rme::testing::CommandStream stream_;
  rme::io::FrameDecoder decoder_{std::size_t{1} << 20U};
  std::vector<std::byte> frame_;
  std::uint64_t session_ = 0;
  std::uint64_t sent_ = 0;
  std::uint64_t acked_ = 0;
  std::uint64_t applied_ = 0;
};

rme::gateway::Endpoint parse(const std::string& text) {
  const std::size_t colon = text.rfind(':');
  return {text.substr(0, colon), static_cast<std::uint16_t>(std::stoul(text.substr(colon + 1)))};
}

rme::gateway::Endpoint find_leader(const std::vector<rme::gateway::Endpoint>& members) {
  rme::gateway::ClientOptions options;
  options.members = members;
  options.nonce = 999'999;
  rme::gateway::Client probe(options);
  probe.open();
  static_cast<void>(probe.submit(rme::engine::Cancel{rme::OrderId(1)}));
  return members.at(probe.target());
}

int run(const std::vector<rme::gateway::Endpoint>& endpoints, double rate, double seconds,
        double warmup, std::uint32_t connections, const std::string& out);

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Open-loop load generator with HdrHistogram latency"};
  std::vector<std::string> members;
  double rate = 10'000;
  double seconds = 10;
  double warmup = 2;
  std::uint32_t connections = 4;
  std::string out;
  app.add_option("--member", members, "Gateway host:port of every member in id order")->required();
  app.add_option("--rate", rate, "Commands per second across all connections");
  app.add_option("--seconds", seconds, "Measured run time");
  app.add_option("--warmup", warmup, "Unrecorded seconds before the measured run");
  app.add_option("--connections", connections, "Pipelined sessions");
  app.add_option("--hgrm", out, "Write the percentile distribution here");
  CLI11_PARSE(app, argc, argv);

  std::vector<rme::gateway::Endpoint> endpoints;
  for (const std::string& member : members) {
    endpoints.push_back(parse(member));
  }
  try {
    return run(endpoints, rate, seconds, warmup, connections, out);
  } catch (const std::exception& error) {
    std::cerr << "loadgen: " << error.what() << '\n';
    return 2;
  }
}

namespace {

int run(const std::vector<rme::gateway::Endpoint>& endpoints, double rate, double seconds,
        double warmup, std::uint32_t connections, const std::string& out) {
  const rme::gateway::Endpoint leader = find_leader(endpoints);
  const auto total = static_cast<std::uint64_t>(rate * (seconds + warmup));
  const std::uint64_t per_stream = total / connections + 1;
  asio::io_context io;
  std::vector<std::unique_ptr<Stream>> streams;
  for (std::uint32_t i = 0; i < connections; ++i) {
    streams.push_back(std::make_unique<Stream>(io, leader, 1 + i, per_stream));
  }
  const auto interval = std::chrono::duration<double>(1.0 / rate);
  const auto start = Clock::now() + std::chrono::milliseconds(100);
  const auto measured_from =
      start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(warmup));
  std::vector<std::uint64_t> sent(connections, 0);
  {
    std::vector<std::jthread> readers;
    for (std::uint32_t i = 0; i < connections; ++i) {
      readers.emplace_back([&, i] {
        const auto deadline =
            start +
            std::chrono::duration_cast<Clock::duration>(interval * static_cast<double>(total)) +
            std::chrono::seconds(5);
        streams[i]->read_until(per_stream, deadline);
      });
    }
    for (std::uint64_t k = 0; k < total; ++k) {
      const auto intended =
          start + std::chrono::duration_cast<Clock::duration>(interval * static_cast<double>(k));
      while (Clock::now() < intended) {
      }
      streams[k % connections]->send(intended, intended >= measured_from);
      ++sent[k % connections];
    }
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    for (std::uint32_t i = 0; i < connections; ++i) {
      while (streams[i]->acked() < sent[i] && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      streams[i]->close();
    }
  }
  Histogram merged = make_histogram();
  std::uint64_t acked = 0;
  std::uint64_t applied = 0;
  for (const auto& stream : streams) {
    hdr_add(merged.get(), stream->histogram());
    acked += stream->acked();
    applied += stream->applied();
  }
  const auto us = [&](double percentile) {
    return static_cast<double>(hdr_value_at_percentile(merged.get(), percentile)) / 1000.0;
  };
  // Under overload the leader refuses proposals; every later seq of that session is then out of
  // order, so only applied commands count.
  std::cout << "rate=" << rate << " sent=" << total << " acked=" << acked << " applied=" << applied
            << " p50_us=" << us(50) << " p99_us=" << us(99) << " p99.9_us=" << us(99.9)
            << " p99.99_us=" << us(99.99)
            << " max_us=" << static_cast<double>(hdr_max(merged.get())) / 1000.0 << '\n';
  if (!out.empty()) {
    if (FILE* file = std::fopen(out.c_str(), "w")) {
      hdr_percentiles_print(merged.get(), file, 5, 1000.0, CLASSIC);
      std::fclose(file);
    }
  }
  return applied == total ? 0 : 1;
}

}  // namespace
