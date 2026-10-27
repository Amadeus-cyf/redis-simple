#ifndef REDIS_SIMPLE_UTILS_CHECK_H_
#define REDIS_SIMPLE_UTILS_CHECK_H_

#include <cstdio>
#include <cstdlib>

// Invariant checks in the style of Google's CHECK. The project does not use
// exceptions, so a broken invariant or violated precondition is a programming
// error: report it and abort. Unlike assert(), checks always run, including in
// release builds.

namespace redis_simple::utils {
[[noreturn]] inline void CheckFailed(const char* condition, const char* file,
                                     int line) {
  std::fprintf(stderr, "%s:%d: Check failed: %s\n", file, line, condition);
  std::abort();
}

inline void Check(bool passed, const char* condition, const char* file,
                  int line) {
  if (!passed) {
    CheckFailed(condition, file, line);
  }
}
}  // namespace redis_simple::utils

#define RS_CHECK(condition)                                              \
  ::redis_simple::utils::Check(static_cast<bool>(condition), #condition, \
                               __FILE__, __LINE__)

// Marks code that an exhaustive enum or an earlier check makes unreachable.
#define RS_UNREACHABLE() \
  ::redis_simple::utils::CheckFailed("unreachable", __FILE__, __LINE__)

#endif  // REDIS_SIMPLE_UTILS_CHECK_H_
