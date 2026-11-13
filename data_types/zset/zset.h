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
#include "memory/listpack.h"

namespace redis_simple::zset {
class ZSet {
 public:
  enum class Encoding {
    kListpack,
    kSkiplist,
  };

  // Redis 8's zset-max-listpack-entries and zset-max-listpack-value.
  static constexpr size_t kListpackMaxEntries = 128;
  static constexpr size_t kListpackMaxElementLength = 64;

  static std::unique_ptr<ZSet> Create() {
    return std::unique_ptr<ZSet>(new ZSet());
  }
  // An empty sorted set in the encoding `count` members need, as Redis'
  // rdbLoad creates one: a skiplist with its dict sized for them when
  // `count` is over kListpackMaxEntries, so loading does not fill a
  // listpack first.
  static std::unique_ptr<ZSet> CreateForSize(size_t count);
  // A listpack-encoded sorted set holding `listpack`'s member and score
  // pairs, as the RDB loader checks them: distinct members no longer than
  // kListpackMaxElementLength, at most kListpackMaxEntries pairs, and scores
  // that are not NaN, in ascending (score, member) order.
  static std::unique_ptr<ZSet> CreateFromListpack(
      std::unique_ptr<memory::Listpack> listpack);
  // Returns true if `member` is new. `*changed`, when given, reports whether it
  // was inserted or its score changed.
  bool InsertOrUpdate(std::string_view member, double score,
                      bool* changed = nullptr);
  bool Delete(std::string_view member);
  std::optional<double> Score(std::string_view member) const;
  std::optional<size_t> Rank(std::string_view member) const;
  // Ranges by rank, score, and member name: `visitor(member, score)` is
  // called for each member in the range's order, with a member view that is
  // valid only during the call. Commands encode replies straight from these.
  void ForEachInRankRange(const RangeByRankSpec* spec,
                          const ZSetRangeVisitor& visitor) const {
    storage_->ForEachInRankRange(spec, visitor);
  }
  void ForEachInScoreRange(const RangeByScoreSpec* spec,
                           const ZSetRangeVisitor& visitor) const {
    storage_->ForEachInScoreRange(spec, visitor);
  }
  void ForEachInLexRange(const RangeByLexSpec* spec,
                         const ZSetRangeVisitor& visitor) const {
    storage_->ForEachInLexRange(spec, visitor);
  }
  // The same ranges copied into vectors, for tests and tools.
  ZSetEntryList RangeByRank(const RangeByRankSpec* spec) const {
    return storage_->RangeByRank(spec);
  }
  ZSetEntryList RangeByScore(const RangeByScoreSpec* spec) const {
    return storage_->RangeByScore(spec);
  }
  ZSetEntryList RangeByLex(const RangeByLexSpec* spec) const {
    return storage_->RangeByLex(spec);
  }
  size_t Count(const RangeByScoreSpec* spec) const;
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
  std::optional<std::string_view> ListpackBytes() const {
    return storage_->ListpackBytes();
  }
  Encoding Encoding() const;

 private:
  ZSet();
  bool ShouldConvertToSkiplist(std::string_view member, bool inserted) const;
  void ConvertAndExpand();
  enum Encoding encoding_;
  std::unique_ptr<ZSetStorage> storage_;
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_H_
