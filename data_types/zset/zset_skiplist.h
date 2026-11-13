#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_

#include <compare>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "data_types/zset/zset_storage.h"
#include "memory/dict.h"
#include "memory/skiplist.h"

namespace redis_simple::zset {
class ZSetSkiplist : public ZSetStorage {
 public:
  // `capacity` sizes the member dict for that many members.
  explicit ZSetSkiplist(size_t capacity = 0);
  bool InsertOrUpdate(std::string_view member, double score,
                      bool* changed) override;
  bool Delete(std::string_view member) override;
  std::optional<double> Score(std::string_view member) const override {
    const double* score = dict_->FindValue(member);
    return score == nullptr ? std::nullopt : std::optional<double>(*score);
  }
  std::optional<size_t> Rank(std::string_view member) const override;
  void ForEachInRankRange(const RangeByRankSpec* spec,
                          const ZSetRangeVisitor& visitor) const override;
  void ForEachInScoreRange(const RangeByScoreSpec* spec,
                           const ZSetRangeVisitor& visitor) const override;
  size_t Count(const RangeByScoreSpec* spec) const override;
  void ForEachInLexRange(const RangeByLexSpec* spec,
                         const ZSetRangeVisitor& visitor) const override;
  size_t LexCount(const RangeByLexSpec* spec) const override;
  bool ForEachEntry(const ZSetEntryVisitor& visitor) const override;
  size_t Scan(size_t cursor, size_t count,
              const ZSetScanVisitor& visitor) override;
  std::optional<std::string_view> ListPackBytes() const override {
    return std::nullopt;
  }
  size_t Size() const override { return skiplist_->Size(); }

 private:
  // Looks up a member by score and name without copying the name.
  struct MemberRef {
    double score;
    std::string_view member;
  };

  // Orders entries by score, then by member name. NaN scores are rejected on
  // insert, so this is a total order. Comparing against a bare score looks at
  // the score only, which lets score ranges search the skiplist directly.
  struct EntryOrder {
    std::partial_ordering operator()(const ZSetEntry& entry,
                                     const MemberRef& member) const {
      if (const std::partial_ordering order = entry.score <=> member.score;
          order != 0) {
        return order;
      }
      return std::string_view(entry.member) <=> member.member;
    }
    std::partial_ordering operator()(const ZSetEntry& left,
                                     const ZSetEntry& right) const {
      return (*this)(left,
                     MemberRef{.score = right.score, .member = right.member});
    }
    std::partial_ordering operator()(const ZSetEntry& entry,
                                     double score) const {
      return entry.score <=> score;
    }
    // Comparing against a lexicographic bound looks at the member only, as
    // Redis' zslFirstInLexRange does while descending the skiplist.
    std::partial_ordering operator()(const ZSetEntry& entry,
                                     const LexBound& bound) const {
      return CompareLex(entry.member, bound);
    }
  };

  using SkiplistType = memory::Skiplist<ZSetEntry, EntryOrder>;

  bool OverlapsLexRange(const RangeByLexSpec* spec) const;
  static std::optional<SkiplistType::Limit> ToLimit(
      const std::optional<LimitSpec>& limit);

  // Maps each member to its score.
  std::unique_ptr<memory::Dict<std::string, double>> dict_;
  // Entries ordered by score, then member name.
  std::unique_ptr<SkiplistType> skiplist_;
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_
