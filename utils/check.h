#ifndef REDIS_SIMPLE_UTILS_CHECK_H_
#define REDIS_SIMPLE_UTILS_CHECK_H_

#include <cstdio>
#include <cstdlib>
#include <source_location>

// Invariant checks in the style of Google's CHECK. The project does not use
// exceptions, so a broken invariant or violated precondition is a programming
// error: report it and abort. Unlike assert(), checks always run, including in
// release builds.

namespace redis_simple::utils {
[[noreturn]] inline void CheckFailed(const char* condition,
                                     const std::source_location& location) {
  std::fprintf(stderr, "%s:%u: Check failed: %s\n", location.file_name(),
               static_cast<unsigned>(location.line()), condition);
  std::abort();
}

// Prefer RS_CHECK, which also reports the condition's source text.
inline void Check(
    bool passed, const char* condition,
    const std::source_location& location = std::source_location::current()) {
  if (!passed) {
    CheckFailed(condition, location);
  }
}

// Marks code that an exhaustive enum or an earlier check makes unreachable.
[[noreturn]] inline void Unreachable(
    const std::source_location& location = std::source_location::current()) {
  CheckFailed("unreachable", location);
}
}  // namespace redis_simple::utils

// std::source_location supplies the caller's file and line; only turning the
// condition into text still needs the preprocessor.
#define RS_CHECK(condition) \
  ::redis_simple::utils::Check(static_cast<bool>(condition), #condition)

#endif  // REDIS_SIMPLE_UTILS_CHECK_H_
