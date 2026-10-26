#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "data_types/zset/zset_entry.h"
#include "data_types/zset/zset_range_spec.h"
#include "data_types/zset/zset_storage.h"

namespace redis_simple::zset {
class ZSet {
 public:
  enum class Encoding {
    kListPack,
    kSkiplist,
  };

  static std::unique_ptr<ZSet> Create() {
    return std::unique_ptr<ZSet>(new ZSet());
  }
  bool InsertOrUpdate(std::string_view key, double score);
  bool Delete(std::string_view key);
  std::optional<double> Score(std::string_view key) const;
  std::optional<size_t> Rank(std::string_view key) const;
  ZSetEntryList RangeByRank(const RangeByRankSpec* spec) const;
  ZSetEntryList RangeByScore(const RangeByScoreSpec* spec) const;
  size_t Count(const RangeByScoreSpec* spec) const;
  bool ForEachEntry(const ZSetEntryVisitor& visitor) const;
  size_t Size() const { return storage_->Size(); }
  Encoding Encoding() const;

 private:
  static constexpr size_t kListPackMaxEntries = 128;
  static constexpr size_t kListPackMaxElementLength = 64;
  ZSet();
  bool ShouldConvertToSkiplist(std::string_view key, bool inserted) const;
  void ConvertAndExpand();
  enum Encoding encoding_;
  std::unique_ptr<ZSetStorage> storage_;
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_H_
