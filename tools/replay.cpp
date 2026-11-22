#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <vector>

#include "engine/book_hash.hpp"
#include "engine/order_book.hpp"
#include "log/command_log.hpp"

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  if (args.size() != 2) {
    std::cerr << "usage: replay <log>\n";
    return 2;
  }
  try {
    rme::log::LogReader reader(args[1]);
    rme::engine::OrderBook book;
    std::vector<rme::engine::Event> events;
    std::uint64_t commands = 0;
    std::uint64_t emitted = 0;
    while (const auto command = reader.next()) {
      events.clear();
      book.apply(*command, events);
      ++commands;
      emitted += events.size();
    }
    std::cout << "commands=" << commands << " events=" << emitted << " resting=" << book.size()
              << " book_hash=" << std::hex << rme::engine::book_hash(book) << '\n';
  } catch (const std::exception& e) {
    std::cerr << "replay: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
