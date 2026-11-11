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
  // Returns true if `member` is new. `*changed`, when given, reports whether it
  // was inserted or its score changed.
  bool InsertOrUpdate(std::string_view member, double score,
                      bool* changed = nullptr);
  bool Delete(std::string_view member);
  std::optional<double> Score(std::string_view member) const;
  std::optional<size_t> Rank(std::string_view member) const;
  ZSetEntryList RangeByRank(const RangeByRankSpec* spec) const;
  ZSetEntryList RangeByScore(const RangeByScoreSpec* spec) const;
  size_t Count(const RangeByScoreSpec* spec) const;
  ZSetEntryList RangeByLex(const RangeByLexSpec* spec) const {
    return storage_->RangeByLex(spec);
  }
  size_t LexCount(const RangeByLexSpec* spec) const {
    return storage_->LexCount(spec);
  }
  bool ForEachEntry(const ZSetEntryVisitor& visitor) const;
  // One ZSCAN step, as Redis runs it: calls `visitor(member, score)` for
  // each member it visits and returns the next cursor, 0 when done. A
  // skiplist-encoded set steps through its member dict
  // (memory::Dict::ScanStep); a listpack is small, so it is visited whole,
  // whatever the cursor and `count`, and returns 0.
  size_t Scan(size_t cursor, size_t count, const ZSetScanVisitor& visitor) {
    return storage_->Scan(cursor, count, visitor);
  }
  size_t Size() const { return storage_->Size(); }
  // A listpack-encoded set's listpack, in Redis' byte format, as RDB files
  // store it; nullopt for a skiplist.
  std::optional<std::string_view> ListPackBytes() const {
    return storage_->ListPackBytes();
  }
  Encoding Encoding() const;

 private:
  static constexpr size_t kListPackMaxEntries = 128;
  static constexpr size_t kListPackMaxElementLength = 64;
  ZSet();
  bool ShouldConvertToSkiplist(std::string_view member, bool inserted) const;
  void ConvertAndExpand();
  enum Encoding encoding_;
  std::unique_ptr<ZSetStorage> storage_;
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_H_
