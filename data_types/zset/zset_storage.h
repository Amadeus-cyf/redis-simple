#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data_types/zset/zset_entry.h"
#include "data_types/zset/zset_range_spec.h"

namespace redis_simple::zset {
using ZSetEntryList = std::vector<const ZSetEntry*>;
using ZSetEntryVisitor =
    std::function<bool(std::string_view key, double score)>;

class ZSetStorage {
 public:
  // Insert or update a key with the given score. Return true if the key is
  // newly inserted.
  virtual bool InsertOrUpdate(std::string_view key, double score) = 0;
  // Return true if the key is deleted.
  virtual bool Delete(std::string_view key) = 0;
  // Return the score of the given key.
  virtual std::optional<double> Score(std::string_view key) const = 0;
  // Return the index of the given key.
  virtual std::optional<size_t> Rank(std::string_view key) const = 0;
  // Return a list of keys within the given index range.
  virtual ZSetEntryList RangeByRank(const RangeByRankSpec* spec) const = 0;
  // Get a list of keys within the given score range.
  virtual ZSetEntryList RangeByScore(const RangeByScoreSpec* spec) const = 0;
  // Count the number of keys within the given range of score.
  virtual size_t Count(const RangeByScoreSpec* spec) const = 0;
  // Visit entries without allocating a result container.
  virtual bool ForEachEntry(const ZSetEntryVisitor& visitor) const = 0;
  // Return the total number of keys.
  virtual size_t Size() const = 0;
  virtual ~ZSetStorage() = default;
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_H_
