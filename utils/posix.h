#ifndef REDIS_SIMPLE_UTILS_POSIX_H_
#define REDIS_SIMPLE_UTILS_POSIX_H_

#include <sys/types.h>

#include <cstddef>

namespace redis_simple::utils {
// The most iovecs one readv() or writev() accepts (IOV_MAX), and at most
// INT_MAX, since those calls take the count as an int.
size_t MaxIovCount();

// waitpid(), retried when a signal interrupts it.
pid_t WaitForChild(pid_t child, int* wait_status, int options);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_POSIX_H_
