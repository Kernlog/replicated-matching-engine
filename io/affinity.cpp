#include "io/affinity.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <system_error>

namespace rme::io {

void pin_this_thread(std::optional<int> cpu) {
  if (!cpu) {
    return;
  }
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(*cpu), &set);
  if (const int error = ::pthread_setaffinity_np(::pthread_self(), sizeof set, &set); error != 0) {
    throw std::system_error(error, std::generic_category(), "pthread_setaffinity_np");
  }
#endif
}

void lock_all_memory() {
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
    throw std::system_error(errno, std::generic_category(), "mlockall");
  }
}

}  // namespace rme::io
