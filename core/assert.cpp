#include "core/assert.hpp"

#include <cstdlib>
#include <iostream>

namespace rme::detail {

void assert_fail(const char* expr, std::source_location loc) noexcept {
  std::cerr << loc.file_name() << ':' << loc.line() << ": " << loc.function_name()
            << ": assertion failed: " << expr << '\n';
  std::abort();
}

}  // namespace rme::detail
