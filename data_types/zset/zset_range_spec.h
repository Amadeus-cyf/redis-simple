#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_RANGE_SPEC_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_RANGE_SPEC_H_

#include <cstddef>
#include <cstdint>
#include <optional>

namespace redis_simple::zset {
// Spec for LIMIT flag
struct LimitSpec {
  // 0-based index
  size_t offset;
  // Empty count means an unbounded range.
  std::optional<size_t> count;
  LimitSpec() : offset(0), count(std::nullopt) {}
  LimitSpec(size_t offset, std::optional<size_t> count)
      : offset(offset), count(count) {}
};

// Spec for range by rank
struct RangeByRankSpec {
  RangeByRankSpec() = default;
  RangeByRankSpec(int64_t min, int64_t max, bool minex, bool maxex,
                  std::optional<LimitSpec> limit = std::nullopt,
                  bool reverse = false)
      : min(min),
        max(max),
        minex(minex),
        maxex(maxex),
        limit(limit),
        reverse(reverse) {}
  // 0-based index
  int64_t min{};
  int64_t max{};
  // Are min or max exclusive?
  bool minex{};
  bool maxex{};
  // Starting offset and count
  std::optional<LimitSpec> limit;
  // Reverse order?
  bool reverse{};
};

// Spec for range by score
struct RangeByScoreSpec {
  RangeByScoreSpec() = default;
  RangeByScoreSpec(double min, double max, bool minex, bool maxex,
                   std::optional<LimitSpec> limit = std::nullopt,
                   bool reverse = false)
      : min(min),
        max(max),
        minex(minex),
        maxex(maxex),
        limit(limit),
        reverse(reverse) {}
  double min{};
  double max{};
  // Are min or max exclusive?
  bool minex{};
  bool maxex{};
  // Starting offset and count
  std::optional<LimitSpec> limit;
  // Reverse order?
  bool reverse{};
};

std::optional<RangeByRankSpec> NormalizeRankRange(const RangeByRankSpec& spec,
                                                  size_t size);
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_RANGE_SPEC_H_
