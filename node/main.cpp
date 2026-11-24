#include <pthread.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <CLI/CLI.hpp>
#include <csignal>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/gateway.hpp"
#include "io/affinity.hpp"
#include "md/publisher.hpp"
#include "node/node.hpp"

namespace {

rme::io::PeerAddress parse_member(std::uint32_t id, std::string_view text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0) {
    throw CLI::ValidationError("--member", "expected host:port, got " + std::string(text));
  }
  return {.id = rme::consensus::NodeId(id),
          .host = std::string(text.substr(0, colon)),
          .port = static_cast<std::uint16_t>(std::stoul(std::string(text.substr(colon + 1))))};
}

sigset_t shutdown_signals() {
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  return signals;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Replicated matching engine node"};
  std::uint32_t id = 0;
  std::string data_dir;
  std::vector<std::string> members;
  rme::node::NodeConfig config;
  std::uint32_t tick_ms = 10;
  std::uint16_t client_port = 7000;
  std::string level = "info";
  std::string md_target;
  app.set_config("--config", "", "TOML or INI file with these options");
  app.add_option("--id", id, "This node's id, its position in --member")->required();
  app.add_option("--data-dir", data_dir, "Directory for the WAL, hard state and snapshot")
      ->required();
  app.add_option("--member", members, "host:port of every member in id order")->required();
  app.add_option("--listen", config.listen_host, "Address to accept peers on");
  app.add_option("--client-port", client_port, "Port the client gateway listens on");
  app.add_option("--tick-ms", tick_ms, "Raft tick interval")->check(CLI::Range(1U, 1000U));
  app.add_option("--election-ticks", config.raft.election_ticks, "Ticks before an election")
      ->check(CLI::Range(2U, 1000U));
  app.add_option("--md-target", md_target, "ip:port to publish the sequenced market data feed to");
  std::vector<int> cpus;
  bool lock_memory = false;
  app.add_option("--cpus", cpus, "CPUs for the core, network and storage threads")->expected(3);
  app.add_flag("--busy-poll", config.busy_poll, "Spin the core thread instead of sleeping");
  app.add_flag("--mlock", lock_memory, "Lock all memory after startup (needs IPC_LOCK)");
  app.add_option("--log-level", level, "trace, debug, info, warn or error");
  CLI11_PARSE(app, argc, argv);

  spdlog::init_thread_pool(8192, 1);
  spdlog::set_default_logger(spdlog::create_async<spdlog::sinks::stdout_color_sink_mt>("rme"));
  spdlog::set_level(spdlog::level::from_str(level));
  spdlog::flush_on(spdlog::level::info);

  // Signals are taken synchronously on this thread, so every thread the node starts blocks them.
  const sigset_t signals = shutdown_signals();
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  int code = 0;
  try {
    config.id = rme::consensus::NodeId(id);
    config.data_dir = data_dir;
    config.tick = std::chrono::milliseconds(tick_ms);
    for (std::size_t i = 0; i < members.size(); ++i) {
      config.members.push_back(parse_member(static_cast<std::uint32_t>(i + 1), members[i]));
    }
    if (cpus.size() == 3) {
      config.core_cpu = cpus[0];
      config.network_cpu = cpus[1];
      config.storage_cpu = cpus[2];
    }
    rme::gateway::Gateway gateway(config.listen_host, client_port);
    rme::node::Node node(config, gateway.outcome_handler());
    std::optional<rme::md::Publisher> publisher;
    if (!md_target.empty()) {
      const rme::io::PeerAddress target = parse_member(0, md_target);
      publisher.emplace(target.host, target.port);
      node.on_entry([&publisher](const rme::consensus::Entry& entry) {
        publisher->publish(entry.index.value(),
                           entry.data);  // NOLINT(bugprone-unchecked-optional-access)
      });
    }
    if (lock_memory) {
      rme::io::lock_all_memory();
    }
    node.start();
    gateway.start(node);
    spdlog::info("node {} started with {} members, data in {}", id, members.size(), data_dir);
    int signal = 0;
    sigwait(&signals, &signal);
    spdlog::info("signal {}: draining", signal);
    gateway.stop();
    node.stop();
    spdlog::info("node {} stopped cleanly", id);
  } catch (const std::exception& error) {
    spdlog::critical("{}", error.what());
    code = 1;
  }
  spdlog::shutdown();
  return code;
}
