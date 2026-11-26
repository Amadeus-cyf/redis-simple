#ifndef REDIS_SIMPLE_UTILS_INLINING_H_
#define REDIS_SIMPLE_UTILS_INLINING_H_

#include <version>

// RS_INLINE_LIBSTDCXX_CALLS, put before a statement, makes clang inline the
// calls in that statement when the standard library is libstdc++, whatever
// its inliner estimates they cost. It is for a libstdc++ call on a hot path
// that a measurement shows clang leaving out of line: the library's
// definition cannot carry an attribute, so the statement that calls it does.
// Say at the statement what was measured, and with which compiler.
//
// It expands to nothing:
// - with libc++, whose definitions clang's inliner weighs differently.
//   Forcing the reply encoders' calls there made an array of bulk strings
//   about a third slower to encode (Apple clang 21, arm64).
// - before clang 15, where [[clang::always_inline]] is a function attribute
//   only, and a statement carrying it does not compile with warnings as
//   errors.
// - with other compilers, which inline as they see fit.
#if defined(__clang__) && __clang_major__ >= 15 && defined(__GLIBCXX__) && \
    __has_cpp_attribute(clang::always_inline)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): a statement attribute
#define RS_INLINE_LIBSTDCXX_CALLS [[clang::always_inline]]
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): a statement attribute
#define RS_INLINE_LIBSTDCXX_CALLS
#endif

#endif  // REDIS_SIMPLE_UTILS_INLINING_H_
