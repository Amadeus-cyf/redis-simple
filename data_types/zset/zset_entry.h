#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_ENTRY_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_ENTRY_H_

#include <string>
#include <string_view>

namespace redis_simple::zset {
// Entry storing key and score
struct ZSetEntry {
  ZSetEntry() = default;
  ZSetEntry(const std::string& key, double score) : key(key), score(score) {}
  ZSetEntry(std::string_view key, double score)
      : key(key.data(), key.size()), score(score) {}
  std::string key;
  double score{};
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_ENTRY_H_
