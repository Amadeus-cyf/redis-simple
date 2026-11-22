#ifndef REDIS_SIMPLE_UTILS_CACHE_LINE_H_
#define REDIS_SIMPLE_UTILS_CACHE_LINE_H_

#include <cstddef>

namespace redis_simple::utils {
// The alignment, and size, that keeps a variable off the cache lines of its
// neighbours. It is for a variable one thread reads often while another
// thread keeps writing what would otherwise sit beside it: each such write
// takes the line away from the reader, whose next read then misses.
//
// Cache lines are 64 bytes on most x86-64 and arm64 processors and 128 on
// Apple's. 128 covers both, and on x86-64 also keeps the variable out of the
// pair of lines that the adjacent-line prefetcher fetches together.
// std::hardware_destructive_interference_size is not used: GCC warns about
// it in a header (-Winterference-size), because its value follows the
// tuning flags.
inline constexpr size_t kCacheLineBytes = 128;
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_CACHE_LINE_H_
