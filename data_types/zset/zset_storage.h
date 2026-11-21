#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data_types/zset/zset_entry.h"
#include "data_types/zset/zset_range_spec.h"

namespace redis_simple::data_types {
// A range collected into a vector of copies: a convenience for tests and
// tools. Commands stream ranges through the ForEachIn...Range visitors.
using ZSetEntryList = std::vector<ZSetEntry>;
// A member's place in score order, with its score.
struct ZSetRank {
  size_t rank{};
  double score{};
};
// Receives each entry of a whole-set walk, in order; returning false stops
// the walk. The member view is valid only during the call.
using ZSetEntryVisitor =
    std::function<bool(std::string_view member, double score)>;
// Receives each member a range or scan step visits, in order. The member
// view is valid only during the call.
using ZSetMemberVisitor =
    std::function<void(std::string_view member, double score)>;

// ZADD's rules for one score-member pair. With none set, the member is added
// or its score replaced.
struct ZSetAddRules {
  // Leave an existing member alone.
  bool nx{};
  // Do not add a new member.
  bool xx{};
  // Change an existing member's score only to a greater (GT) or a smaller
  // (LT) one. Neither stops a new member from being added.
  bool gt{};
  bool lt{};
  // Add the score to the member's current score instead of replacing it.
  bool incr{};
};

enum class ZSetAddStatus : uint8_t {
  kAdded,
  kUpdated,
  // Processed without a change: the score was already that value.
  kUnchanged,
  // Left alone because of NX, XX, GT, or LT.
  kSkipped,
  // The score, or INCR's sum, is NaN. Nothing changed.
  kNotANumber,
};

struct ZSetAddResult {
  ZSetAddStatus status{};
  // The member's score after the call, when it was added, updated, or
  // unchanged.
  double score{};
};

// The part of Redis' zsetAdd for a member that exists with score `current`:
// applies NX, INCR, and GT/LT in that order, and reports the score the member
// should have. The caller stores it when the status is kUpdated.
inline ZSetAddResult ApplyAddRulesToExisting(double current, double score,
                                             const ZSetAddRules& rules) {
  if (rules.nx) {
    return {.status = ZSetAddStatus::kSkipped, .score = current};
  }
  if (rules.incr) {
    score += current;
    if (std::isnan(score)) {
      return {.status = ZSetAddStatus::kNotANumber, .score = score};
    }
  }
  if ((rules.lt && score >= current) || (rules.gt && score <= current)) {
    return {.status = ZSetAddStatus::kSkipped, .score = current};
  }
  return {.status = score == current ? ZSetAddStatus::kUnchanged
                                     : ZSetAddStatus::kUpdated,
          .score = score};
}

// What one sorted set encoding (listpack or skiplist) implements. ZSet
// forwards to whichever currently holds the members.
class ZSetStorage {
 public:
  // Redis' zsetAdd: adds `member`, or changes its score, as `rules` allow,
  // looking the member up once.
  virtual ZSetAddResult Add(std::string_view member, double score,
                            const ZSetAddRules& rules) = 0;
  // Insert or update a member with the given score. Return true if the member
  // is newly inserted. `*changed`, when given, reports whether the member was
  // inserted or its score changed.
  bool InsertOrUpdate(std::string_view member, double score, bool* changed) {
    const ZSetAddStatus status = Add(member, score, ZSetAddRules{}).status;
    if (changed != nullptr) {
      *changed =
          status == ZSetAddStatus::kAdded || status == ZSetAddStatus::kUpdated;
    }
    return status == ZSetAddStatus::kAdded;
  }
  // Return true if the member is deleted.
  virtual bool Delete(std::string_view member) = 0;
  // Return the score of the given member.
  virtual std::optional<double> Score(std::string_view member) const = 0;
  // The member's rank and score from one lookup, as ZRANK ... WITHSCORE
  // needs them.
  virtual std::optional<ZSetRank> RankAndScore(
      std::string_view member) const = 0;
  // Visit the members within the given index range, without copying them.
  virtual void ForEachInRankRange(const RangeByRankSpec* spec,
                                  const ZSetMemberVisitor& visitor) const = 0;
  // Visit the members within the given score range.
  virtual void ForEachInScoreRange(const RangeByScoreSpec* spec,
                                   const ZSetMemberVisitor& visitor) const = 0;
  // Count the number of members within the given range of score.
  virtual size_t Count(const RangeByScoreSpec* spec) const = 0;
  // Visit the members in a lexicographic range, as Redis' ZRANGEBYLEX does.
  virtual void ForEachInLexRange(const RangeByLexSpec* spec,
                                 const ZSetMemberVisitor& visitor) const = 0;
  // Count the members in a lexicographic range, ignoring its limit.
  virtual size_t LexCount(const RangeByLexSpec* spec) const = 0;
  // Visit entries without allocating a result container.
  virtual bool ForEachEntry(const ZSetEntryVisitor& visitor) const = 0;
  // One ZSCAN step from `cursor`: calls `visitor(member, score)` for each
  // member it visits and returns the next cursor, 0 when done. See
  // ZSet::Scan.
  virtual size_t Scan(size_t cursor, size_t count,
                      const ZSetMemberVisitor& visitor) = 0;
  // Return the total number of members.
  virtual size_t Size() const = 0;
  // A listpack storage's listpack, in Redis' byte format; nullopt for a
  // skiplist.
  virtual std::optional<std::string_view> ListpackBytes() const = 0;
  // See ZSet::ClearSome. A listpack frees at once.
  virtual bool ClearSome(size_t /*max_elements*/) { return true; }
  virtual ~ZSetStorage() = default;

  std::optional<size_t> Rank(std::string_view member) const {
    const std::optional<ZSetRank> found = RankAndScore(member);
    return found.has_value() ? std::optional<size_t>(found->rank)
                             : std::nullopt;
  }

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
  static ZSetMemberVisitor Collector(ZSetEntryList* const entries) {
    return [entries](std::string_view member, double score) {
      entries->emplace_back(member, score);
    };
  }
};
}  // namespace redis_simple::data_types

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_STORAGE_H_
