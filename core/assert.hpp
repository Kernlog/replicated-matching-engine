#pragma once

#include <source_location>

namespace rme::detail {

[[noreturn]] void assert_fail(const char* expr, std::source_location loc) noexcept;

}  // namespace rme::detail

#define RME_ASSERT(expr)                          \
  (static_cast<bool>(expr) ? static_cast<void>(0) \
                           : ::rme::detail::assert_fail(#expr, std::source_location::current()))
