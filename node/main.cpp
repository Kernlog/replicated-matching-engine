#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "node/node.hpp"

namespace {

rme::io::PeerAddress parse_member(std::uint32_t id, std::string_view text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos) {
    throw std::invalid_argument("member must be host:port");
  }
  return {.id = rme::consensus::NodeId(id),
          .host = std::string(text.substr(0, colon)),
          .port = static_cast<std::uint16_t>(std::stoul(std::string(text.substr(colon + 1))))};
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv, argv + argc);
  if (args.size() < 4) {
    std::cerr << "usage: rme_node <id> <data-dir> <host:port>...\n";
    return 2;
  }
  try {
    rme::node::NodeConfig config;
    config.id =
        rme::consensus::NodeId(static_cast<std::uint32_t>(std::stoul(std::string(args[1]))));
    config.data_dir = args[2];
    for (std::size_t i = 3; i < args.size(); ++i) {
      config.members.push_back(parse_member(static_cast<std::uint32_t>(i - 2), args[i]));
    }
    rme::node::Node node(config);
    node.start();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const rme::node::NodeStatus status = node.status();
      std::cout << "role=" << static_cast<int>(status.role) << " term=" << status.term
                << " leader=" << status.leader << " commit=" << status.commit
                << " applied=" << status.applied << std::endl;
    }
  } catch (const std::exception& error) {
    std::cerr << "rme_node: " << error.what() << '\n';
    return 1;
  }
}
