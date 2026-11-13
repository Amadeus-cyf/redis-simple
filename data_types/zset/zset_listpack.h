#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_LISTPACK_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_LISTPACK_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "data_types/zset/zset_storage.h"
#include "memory/listpack.h"

namespace redis_simple::zset {
class ZSetListPack : public ZSetStorage {
 public:
  ZSetListPack();
  explicit ZSetListPack(std::unique_ptr<memory::ListPack> listpack)
      : listpack_(std::move(listpack)) {}
  bool InsertOrUpdate(std::string_view member, double score,
                      bool* changed) override;
  bool Delete(std::string_view member) override;
  std::optional<double> Score(std::string_view member) const override;
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
  std::optional<std::string_view> ListPackBytes() const override {
    return listpack_->Bytes();
  }
  size_t Scan(size_t cursor, size_t count,
              const ZSetScanVisitor& visitor) override;
  size_t Size() const override {
    // Since listpack include both members and scores, the actual size should be
    // divided by 2.
    return listpack_->Size() / 2;
  }

 private:
  struct EntryView {
    std::string_view member;
    double score;
    size_t member_index;
    size_t score_index;
  };

  void DeleteMemberScorePair(size_t offset);
  std::optional<std::string_view> ValueAt(size_t offset) const;
  std::optional<EntryView> EntryAt(size_t member_offset) const;
  std::optional<double> ScoreAt(size_t offset) const;
  // The forward and reverse walks behind the range visitors. A rank spec
  // must be normalized and valid.
  void VisitRankRange(const RangeByRankSpec* spec,
                      const ZSetRangeVisitor& visitor) const;
  void VisitRankRangeReversed(const RangeByRankSpec* spec,
                              const ZSetRangeVisitor& visitor) const;
  void VisitScoreRange(const RangeByScoreSpec* spec,
                       const ZSetRangeVisitor& visitor) const;
  void VisitScoreRangeReversed(const RangeByScoreSpec* spec,
                               const ZSetRangeVisitor& visitor) const;
  std::optional<size_t> NextMemberAfterScore(size_t score_offset) const;
  std::optional<size_t> PrevScoreBeforeMember(size_t member_offset) const;
  std::optional<size_t> PrevMemberBeforeMember(size_t member_offset) const;
  std::optional<size_t> FindKeyGreaterOrEqual(
      const RangeByScoreSpec* spec) const;
  std::optional<size_t> FindKeyLessOrEqual(const RangeByScoreSpec* spec) const;
  bool OverlapsLexRange(const RangeByLexSpec* spec) const;
  static bool ValidateRangeRankSpec(const RangeByRankSpec* spec);
  static bool ValidateRangeScoreSpec(const RangeByScoreSpec* spec);
  static bool IsInRange(double score, const RangeByScoreSpec* spec);
  static bool GreaterOrEqual(double score, const RangeByScoreSpec* spec);
  static bool LessOrEqual(double score, const RangeByScoreSpec* spec);
  // Listpack storing member score pairs
  std::unique_ptr<memory::ListPack> listpack_;
};
}  // namespace redis_simple::zset

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_LISTPACK_H_
