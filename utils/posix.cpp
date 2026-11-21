#include "utils/posix.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>

namespace redis_simple::utils {
size_t MaxIovCount() {
  static const size_t kMaxIovCount = [] {
    const int64_t system_limit = sysconf(_SC_IOV_MAX);
    const size_t limit =
        system_limit > 0 ? static_cast<size_t>(system_limit) : 1024;
    return std::min(limit,
                    static_cast<size_t>(std::numeric_limits<int>::max()));
  }();
  return kMaxIovCount;
}

pid_t WaitForChild(pid_t child, int* const wait_status, int options) {
  while (true) {
    const pid_t result = waitpid(child, wait_status, options);
    if (result >= 0 || errno != EINTR) {
      return result;
    }
  }
}
}  // namespace redis_simple::utils
