#include <charconv>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

#include "sim/cluster.hpp"
#include "sim/swarm.hpp"

namespace {

bool parse(std::string_view text, std::uint64_t& out) {
  const auto [end, error] = std::from_chars(text.begin(), text.end(), out);
  return error == std::errc{} && end == text.end();
}

bool parse_range(std::string_view text, std::uint64_t& first, std::uint64_t& last) {
  const auto dots = text.find("..");
  if (dots == std::string_view::npos) {
    const bool ok = parse(text, first);
    last = first;
    return ok;
  }
  return parse(text.substr(0, dots), first) && parse(text.substr(dots + 2), last) && first <= last;
}

void report(std::uint64_t seed, const rme::sim::SimConfig& config, const rme::sim::SimStats& stats,
            bool verbose) {
  std::cout << "seed=" << seed << " trace=" << std::hex << stats.trace_hash << std::dec
            << " delivered=" << stats.delivered << " proposed=" << stats.proposed
            << " applied=" << stats.applied << " crashes=" << stats.crashes
            << " sync_crashes=" << stats.sync_crashes
            << " recovered_after=" << stats.recovered_after.value()
            << (stats.violations.empty() ? " ok" : " FAIL") << '\n';
  if (verbose || !stats.violations.empty()) {
    std::cout << "  plan: " << rme::sim::describe(config) << '\n';
  }
  for (const auto& violation : stats.violations) {
    std::cout << "  " << violation << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  const bool verbose = args.size() == 4 && std::string_view(args[3]) == "--verbose";
  if ((args.size() != 3 && !verbose) || std::string_view(args[1]) != "--seeds" ||
      !parse_range(args[2], first, last)) {
    std::cerr << "usage: sim --seeds <n>|<first>..<last> [--verbose]\n";
    return 2;
  }
  std::uint64_t failed = 0;
  for (std::uint64_t seed = first; seed <= last; ++seed) {
    const rme::sim::SimConfig config = rme::sim::swarm(seed);
    const rme::sim::SimStats stats = rme::sim::Cluster(config).run();
    report(seed, config, stats, verbose);
    failed += stats.violations.empty() ? 0U : 1U;
  }
  std::cout << "seeds=" << (last - first + 1) << " failed=" << failed << '\n';
  return failed == 0 ? 0 : 1;
}
