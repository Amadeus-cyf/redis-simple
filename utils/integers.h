#ifndef REDIS_SIMPLE_UTILS_INTEGERS_H_
#define REDIS_SIMPLE_UTILS_INTEGERS_H_

#include <cstdint>

namespace redis_simple::utils {
// How many decimal digits `value` has: 1 for 0 to 9, up to 20.
uint32_t Digits10(uint64_t value);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_INTEGERS_H_
