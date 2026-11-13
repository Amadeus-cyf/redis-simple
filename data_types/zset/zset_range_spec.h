#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_RANGE_SPEC_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_RANGE_SPEC_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace redis_simple::data_types {
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

// One end of a lexicographic range: a member, or Redis' "-" and "+", which
// sort before and after every member.
struct LexBound {
  enum class Kind : std::uint8_t {
    kValue,
    kMinusInfinity,
    kPlusInfinity,
  };
  Kind kind{Kind::kValue};
  // Views the command argument, without its "[" or "(".
  std::string_view value;
};

// Redis' sdscmplex for a member against a bound.
inline std::strong_ordering CompareLex(std::string_view member,
                                       const LexBound& bound) {
  switch (bound.kind) {
    case LexBound::Kind::kMinusInfinity:
      return std::strong_ordering::greater;
    case LexBound::Kind::kPlusInfinity:
      return std::strong_ordering::less;
    case LexBound::Kind::kValue:
      break;
  }
  return member <=> bound.value;
}

// Spec for a range by member name, which Redis defines for members that
// share one score.
struct RangeByLexSpec {
  LexBound min;
  LexBound max;
  bool minex{};
  bool maxex{};
  std::optional<LimitSpec> limit;
  bool reverse{};

  bool AboveMin(std::string_view member) const {
    const std::strong_ordering order = CompareLex(member, min);
    return minex ? order > 0 : order >= 0;
  }
  bool BelowMax(std::string_view member) const {
    const std::strong_ordering order = CompareLex(member, max);
    return maxex ? order < 0 : order <= 0;
  }
  // Redis' zslIsInLexRange check that min does not exceed max.
  bool Empty() const {
    std::strong_ordering order = std::strong_ordering::equal;
    if (min.kind != max.kind) {
      order = min.kind == LexBound::Kind::kMinusInfinity ||
                      max.kind == LexBound::Kind::kPlusInfinity
                  ? std::strong_ordering::less
                  : std::strong_ordering::greater;
    } else if (min.kind == LexBound::Kind::kValue) {
      order = min.value <=> max.value;
    }
    return order > 0 || (order == 0 && (minex || maxex));
  }
};

std::optional<RangeByRankSpec> NormalizeRankRange(const RangeByRankSpec& spec,
                                                  size_t size);
}  // namespace redis_simple::data_types

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_RANGE_SPEC_H_
