#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_

#include <compare>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "containers/dict.h"
#include "containers/skiplist.h"
#include "data_types/zset/zset_storage.h"

namespace redis_simple::data_types {
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
  std::optional<ZSetRank> RankAndScore(std::string_view member) const override;
  void ForEachInRankRange(const RangeByRankSpec* spec,
                          const ZSetMemberVisitor& visitor) const override;
  void ForEachInScoreRange(const RangeByScoreSpec* spec,
                           const ZSetMemberVisitor& visitor) const override;
  size_t Count(const RangeByScoreSpec* spec) const override;
  void ForEachInLexRange(const RangeByLexSpec* spec,
                         const ZSetMemberVisitor& visitor) const override;
  size_t LexCount(const RangeByLexSpec* spec) const override;
  bool ForEachEntry(const ZSetEntryVisitor& visitor) const override;
  size_t Scan(size_t cursor, size_t count,
              const ZSetMemberVisitor& visitor) override;
  std::optional<std::string_view> ListpackBytes() const override {
    return std::nullopt;
  }
  size_t Size() const override { return skiplist_->Size(); }

  // The same ranges for a visitor of any callable type, which is called
  // directly. Commands use these through ZSet, so a large range costs no
  // indirect call per member.
  template <typename Visitor>
  void VisitRankRange(const RangeByRankSpec* spec, Visitor&& visitor) const;
  template <typename Visitor>
  void VisitScoreRange(const RangeByScoreSpec* spec, Visitor&& visitor) const;
  template <typename Visitor>
  void VisitLexRange(const RangeByLexSpec* spec, Visitor&& visitor) const;

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

  using SkiplistType = containers::Skiplist<ZSetEntry, EntryOrder>;

  bool OverlapsLexRange(const RangeByLexSpec* spec) const;
  static std::optional<SkiplistType::Limit> ToLimit(
      const std::optional<LimitSpec>& limit);

  // Maps each member to its score.
  std::unique_ptr<containers::Dict<std::string, double>> dict_;
  // Entries ordered by score, then member name.
  std::unique_ptr<SkiplistType> skiplist_;
};
template <typename Visitor>
void ZSetSkiplist::VisitRankRange(const RangeByRankSpec* spec,
                                  Visitor&& visitor) const {
  if (spec == nullptr) {
    return;
  }
  const std::optional<RangeByRankSpec> normalized =
      NormalizeRankRange(*spec, Size());
  if (!normalized.has_value()) {
    return;
  }
  const SkiplistType::RankRange range{
      .min = static_cast<size_t>(normalized->min),
      .max = static_cast<size_t>(normalized->max),
      .min_exclusive = normalized->minex,
      .max_exclusive = normalized->maxex,
      .limit = ToLimit(normalized->limit),
      .reverse = spec->reverse,
  };
  skiplist_->ForEachInRange(range, [&visitor](const ZSetEntry& entry) {
    visitor(entry.member, entry.score);
  });
}

template <typename Visitor>
void ZSetSkiplist::VisitScoreRange(const RangeByScoreSpec* spec,
                                   Visitor&& visitor) const {
  if (spec == nullptr) {
    return;
  }
  const SkiplistType::KeyRange<double> range{
      .min = spec->min,
      .max = spec->max,
      .min_exclusive = spec->minex,
      .max_exclusive = spec->maxex,
      .limit = ToLimit(spec->limit),
      .reverse = spec->reverse,
  };
  skiplist_->ForEachInRange(range, [&visitor](const ZSetEntry& entry) {
    visitor(entry.member, entry.score);
  });
}

template <typename Visitor>
void ZSetSkiplist::VisitLexRange(const RangeByLexSpec* spec,
                                 Visitor&& visitor) const {
  if (!OverlapsLexRange(spec)) {
    return;
  }
  skiplist_->ForEachInRange(
      SkiplistType::KeyRange<LexBound>{
          .min = spec->min,
          .max = spec->max,
          .min_exclusive = spec->minex,
          .max_exclusive = spec->maxex,
          .limit = ToLimit(spec->limit),
          .reverse = spec->reverse,
      },
      [&visitor](const ZSetEntry& entry) {
        visitor(entry.member, entry.score);
      });
}
}  // namespace redis_simple::data_types

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_
