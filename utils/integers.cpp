#include "utils/integers.h"

#include <cstdint>

namespace redis_simple::utils {
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
  if (value < 1000000000000UL) {
    if (value < 100000000UL) {
      if (value < 1000000) {
        if (value < 10000) {
          return 4;
        }
        return 5 + static_cast<int>(value >= 100000);
      }
      return 7 + static_cast<int>(value >= 10000000UL);
    }
    if (value < 10000000000UL) {
      return 9 + static_cast<int>(value >= 1000000000UL);
    }
    return 11 + static_cast<int>(value >= 100000000000UL);
  }
  return 12 + Digits10(value / 1000000000000UL);
}
}  // namespace redis_simple::utils
