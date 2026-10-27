#ifndef REDIS_SIMPLE_UTILS_INT_UTILS_H_
#define REDIS_SIMPLE_UTILS_INT_UTILS_H_

#include <cstdint>

namespace redis_simple::utils {
// Get number of digits in the number.
uint32_t Digits10(uint64_t v);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_INT_UTILS_H_
