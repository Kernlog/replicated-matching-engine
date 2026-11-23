#pragma once

#include <cstdint>

#include "engine/command.hpp"

namespace rme::engine {

inline constexpr std::uint64_t kRegisterClient = 0;

struct ClientCommand {
  std::uint64_t client = kRegisterClient;
  std::uint64_t seq = 0;
  Command command = Cancel{};

  friend bool operator==(const ClientCommand&, const ClientCommand&) = default;
};

}  // namespace rme::engine
