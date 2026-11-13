#include "data_types/zset/zset_range_spec.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace redis_simple::data_types {
std::optional<RangeByRankSpec> NormalizeRankRange(const RangeByRankSpec& spec,
                                                  size_t size) {
  if (size == 0 ||
      size > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  const auto signed_size = static_cast<int64_t>(size);
  int64_t start = spec.min;
  int64_t stop = spec.max;

  if (start < 0) {
    start = start < -signed_size ? 0 : start + signed_size;
  }
  if (stop < 0) {
    if (stop < -signed_size) {
      return std::nullopt;
    }
    stop += signed_size;
  }
  if (start >= signed_size || start > stop) {
    return std::nullopt;
  }
  stop = std::min(stop, signed_size - 1);

  RangeByRankSpec normalized = spec;
  normalized.min = start;
  normalized.max = stop;
  return normalized;
}
}  // namespace redis_simple::data_types
