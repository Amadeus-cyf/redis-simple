#include "utils/integers.h"

#include <cstdint>

namespace redis_simple::utils {
// A comparison tree rather than a loop: no division for values below 10^12,
// which is nearly every value formatted.
uint32_t Digits10(uint64_t value) {
  if (value < 10) {
    return 1;
  }
  if (value < 100) {
    return 2;
  }
  if (value < 1000) {
    return 3;
  }
  if (value < 1000000000000ULL) {
    if (value < 100000000ULL) {
      if (value < 1000000) {
        if (value < 10000) {
          return 4;
        }
        return 5 + (value >= 100000 ? 1 : 0);
      }
      return 7 + (value >= 10000000ULL ? 1 : 0);
    }
    if (value < 10000000000ULL) {
      return 9 + (value >= 1000000000ULL ? 1 : 0);
    }
    return 11 + (value >= 100000000000ULL ? 1 : 0);
  }
  return 12 + Digits10(value / 1000000000000ULL);
}
}  // namespace redis_simple::utils
