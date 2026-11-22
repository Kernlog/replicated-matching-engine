#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

#include "log/command_log.hpp"
#include "testing/command_stream.hpp"

namespace {

bool parse(std::string_view text, std::uint64_t& out) {
  const auto [end, error] = std::from_chars(text.begin(), text.end(), out);
  return error == std::errc{} && end == text.end();
}

}  // namespace

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  std::uint64_t seed = 0;
  std::uint64_t count = 0;
  if (args.size() != 4 || !parse(args[1], seed) || !parse(args[2], count)) {
    std::cerr << "usage: gen_log <seed> <count> <path>\n";
    return 2;
  }
  try {
    const std::filesystem::path path(args[3]);
    std::filesystem::remove(path);
    rme::testing::CommandStream stream(seed);
    rme::log::LogWriter writer(path);
    for (std::uint64_t i = 0; i < count; ++i) {
      writer.append(stream.next());
    }
    writer.sync();
  } catch (const std::exception& e) {
    std::cerr << "gen_log: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
