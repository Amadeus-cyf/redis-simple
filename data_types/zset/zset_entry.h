#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_ENTRY_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_ENTRY_H_

#include <string>
#include <string_view>

namespace redis_simple::zset {
// Entry storing member and score
struct ZSetEntry {
  ZSetEntry() = default;
  ZSetEntry(const std::string& member, double score)
      : member(member), score(score) {}
  ZSetEntry(std::string_view member, double score)
      : member(member.data(), member.size()), score(score) {}
  std::string member;
  double score{};
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_ENTRY_H_
