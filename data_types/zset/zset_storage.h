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
// A range collected into a vector of copies: a convenience for tests and
// tools. Commands stream ranges through the ForEachIn...Range visitors.
using ZSetEntryList = std::vector<ZSetEntry>;
using ZSetEntryVisitor =
    std::function<bool(std::string_view member, double score)>;
using ZSetScanVisitor =
    std::function<void(std::string_view member, double score)>;
// Receives the members of a range, in the range's order. The member view is
// valid only during the call.
using ZSetRangeVisitor =
    std::function<void(std::string_view member, double score)>;

class ZSetStorage {
 public:
  // Insert or update a member with the given score. Return true if the member
  // is newly inserted. `*changed`, when given, reports whether the member was
  // inserted or its score changed.
  virtual bool InsertOrUpdate(std::string_view member, double score,
                              bool* changed) = 0;
  // Return true if the member is deleted.
  virtual bool Delete(std::string_view member) = 0;
  // Return the score of the given member.
  virtual std::optional<double> Score(std::string_view member) const = 0;
  // Return the index of the given member.
  virtual std::optional<size_t> Rank(std::string_view member) const = 0;
  // Visit the members within the given index range, without copying them.
  virtual void ForEachInRankRange(const RangeByRankSpec* spec,
                                  const ZSetRangeVisitor& visitor) const = 0;
  // Visit the members within the given score range.
  virtual void ForEachInScoreRange(const RangeByScoreSpec* spec,
                                   const ZSetRangeVisitor& visitor) const = 0;
  // Count the number of members within the given range of score.
  virtual size_t Count(const RangeByScoreSpec* spec) const = 0;
  // Visit the members in a lexicographic range, as Redis' ZRANGEBYLEX does.
  virtual void ForEachInLexRange(const RangeByLexSpec* spec,
                                 const ZSetRangeVisitor& visitor) const = 0;
  // Count the members in a lexicographic range, ignoring its limit.
  virtual size_t LexCount(const RangeByLexSpec* spec) const = 0;
  // Visit entries without allocating a result container.
  virtual bool ForEachEntry(const ZSetEntryVisitor& visitor) const = 0;
  // One ZSCAN step from `cursor`: calls `visitor(member, score)` for each
  // member it visits and returns the next cursor, 0 when done. See
  // ZSet::Scan.
  virtual size_t Scan(size_t cursor, size_t count,
                      const ZSetScanVisitor& visitor) = 0;
  // Return the total number of members.
  virtual size_t Size() const = 0;
  // A listpack storage's listpack, in Redis' byte format; nullopt for a
  // skiplist.
  virtual std::optional<std::string_view> ListpackBytes() const = 0;
  virtual ~ZSetStorage() = default;

  // The same ranges collected into vectors.
  ZSetEntryList RangeByRank(const RangeByRankSpec* spec) const {
    ZSetEntryList entries;
    ForEachInRankRange(spec, Collector(&entries));
    return entries;
  }
  ZSetEntryList RangeByScore(const RangeByScoreSpec* spec) const {
    ZSetEntryList entries;
    ForEachInScoreRange(spec, Collector(&entries));
    return entries;
  }
  ZSetEntryList RangeByLex(const RangeByLexSpec* spec) const {
    ZSetEntryList entries;
    ForEachInLexRange(spec, Collector(&entries));
    return entries;
  }

 private:
  static ZSetRangeVisitor Collector(ZSetEntryList* const entries) {
    return [entries](std::string_view member, double score) {
      entries->emplace_back(member, score);
    };
  }
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_H_
