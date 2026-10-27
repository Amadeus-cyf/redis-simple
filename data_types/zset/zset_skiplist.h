#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "data_types/zset/zset_storage.h"
#include "logging/logger.h"
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
  struct Comparator {
    int operator()(const ZSetEntry* s1, const ZSetEntry* s2) const {
      if (s1->score < s2->score) {
        return -1;
      }
      if (s1->score > s2->score) {
        return 1;
      }
      return s1->key.compare(s2->key);
    }
  };

  struct Destructor {
    void operator()(const ZSetEntry* entry) const {
      if (entry == nullptr) {
        return;
      }
      RS_LOG_DEBUG("delete %s %f\n", entry->key.c_str(), entry->score);
      delete entry;
    }
  };

  using SkiplistType =
      in_memory::Skiplist<const ZSetEntry*, Comparator, Destructor>;
  using SkiplistLimitSpec = SkiplistType::SkiplistLimitSpec;
  using SkiplistRangeByRankSpec = SkiplistType::SkiplistRangeByRankSpec;
  using SkiplistRangeByKeySpec = SkiplistType::SkiplistRangeByKeySpec;
  struct RankSpecDeleter {
    void operator()(SkiplistRangeByRankSpec* spec) const;
  };
  struct KeySpecDeleter {
    void operator()(SkiplistRangeByKeySpec* spec) const;
  };
  using RankSpecPtr = std::unique_ptr<SkiplistRangeByRankSpec, RankSpecDeleter>;
  using KeySpecPtr = std::unique_ptr<SkiplistRangeByKeySpec, KeySpecDeleter>;

  RankSpecPtr ToSkiplistRangeByRankSpec(const RangeByRankSpec* spec) const;
  KeySpecPtr ToSkiplistRangeByKeySpec(const RangeByScoreSpec* spec) const;
  void RecomputeMinMaxKeys();
  // Dict mapping key to score, used with the skiplist
  std::unique_ptr<in_memory::Dict<std::string, double>> dict_;
  // Skiplist storing key score pairs ordered by score
  std::unique_ptr<SkiplistType> skiplist_;
  // Min and max key value, used for RangeByScore
  mutable std::optional<std::string> max_key_, min_key_;
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_SKIPLIST_H_
