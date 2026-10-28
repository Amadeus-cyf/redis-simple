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
  ZSetSkiplist();
  bool InsertOrUpdate(std::string_view key, double score) override;
  bool Delete(std::string_view key) override;
  std::optional<double> Score(std::string_view key) const override {
    const auto* score = dict_->FindValue(key);
    return score == nullptr ? std::nullopt : std::optional<double>(*score);
  }
  std::optional<size_t> Rank(std::string_view key) const override;
  ZSetEntryList RangeByRank(const RangeByRankSpec* spec) const override;
  ZSetEntryList RangeByScore(const RangeByScoreSpec* spec) const override;
  size_t Count(const RangeByScoreSpec* spec) const override;
  bool ForEachEntry(const ZSetEntryVisitor& visitor) const override;
  size_t Size() const override { return skiplist_->Size(); }

 private:
  // Looks up a member by score and name without copying the name.
  struct MemberRef {
    double score;
    std::string_view key;
  };

  // Orders entries by score, then by member name. NaN scores are rejected on
  // insert, so this is a total order. Comparing against a bare score looks at
  // the score only, which lets score ranges search the skiplist directly.
  struct EntryOrder {
    std::partial_ordering operator()(const ZSetEntry& entry,
                                     const MemberRef& member) const {
      if (const auto order = entry.score <=> member.score; order != 0) {
        return order;
      }
      return std::string_view(entry.key) <=> member.key;
    }
    std::partial_ordering operator()(const ZSetEntry& left,
                                     const ZSetEntry& right) const {
      return (*this)(left, MemberRef{.score = right.score, .key = right.key});
    }
    std::partial_ordering operator()(const ZSetEntry& entry,
                                     double score) const {
      return entry.score <=> score;
    }
  };

  using SkiplistType = in_memory::Skiplist<ZSetEntry, EntryOrder>;

  static std::optional<SkiplistType::Limit> ToLimit(
      const std::optional<LimitSpec>& limit);

  // Maps each member to its score.
  std::unique_ptr<in_memory::Dict<std::string, double>> dict_;
  // Entries ordered by score, then member name.
  std::unique_ptr<SkiplistType> skiplist_;
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_
