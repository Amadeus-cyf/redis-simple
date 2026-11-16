#ifndef REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_LISTPACK_H_
#define REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_LISTPACK_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "containers/listpack.h"
#include "data_types/zset/zset_storage.h"

namespace redis_simple::data_types {
class ZSetListpack : public ZSetStorage {
 public:
  ZSetListpack();
  // A ZSetListpack without bytes, for a sorted set held in a skiplist.
  static ZSetListpack Unallocated() {
    return ZSetListpack(containers::Listpack::Unallocated());
  }
  bool Allocated() const { return listpack_.Allocated(); }
  void Release() { listpack_.Release(); }
  explicit ZSetListpack(std::unique_ptr<containers::Listpack> listpack)
      : listpack_(std::move(*listpack)) {}
  ZSetAddResult Add(std::string_view member, double score,
                    const ZSetAddRules& rules) override;
  bool Delete(std::string_view member) override;
  std::optional<double> Score(std::string_view member) const override;
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
  std::optional<std::string_view> ListpackBytes() const override {
    return listpack_.Bytes();
  }
  size_t Scan(size_t cursor, size_t count,
              const ZSetMemberVisitor& visitor) override;
  size_t Size() const override {
    // Since listpack include both members and scores, the actual size should be
    // divided by 2.
    return listpack_.Size() / 2;
  }

 private:
  explicit ZSetListpack(containers::Listpack listpack)
      : listpack_(std::move(listpack)) {}
  struct EntryView {
    std::string_view member;
    double score;
    size_t member_index;
    size_t score_index;
  };

  // Inserts the pair at its place in (score, member) order. Returns false,
  // leaving the listpack as it was, if the listpack cannot hold the pair.
  bool InsertSorted(std::string_view member, double score);
  void DeleteMemberScorePair(size_t offset);
  // ValueAt and EntryAt format an integer-encoded member into the caller's
  // buffer, so a view from one stays valid while its buffer does.
  std::optional<std::string_view> ValueAt(
      size_t offset, containers::Listpack::IntBuffer* buffer) const;
  std::optional<EntryView> EntryAt(
      size_t member_offset,
      containers::Listpack::IntBuffer* member_buffer) const;
  std::optional<double> ScoreAt(size_t offset) const;
  // The forward and reverse walks behind the range visitors. A rank spec
  // must be normalized and valid.
  void VisitRankRange(const RangeByRankSpec* spec,
                      const ZSetMemberVisitor& visitor) const;
  void VisitRankRangeReversed(const RangeByRankSpec* spec,
                              const ZSetMemberVisitor& visitor) const;
  void VisitScoreRange(const RangeByScoreSpec* spec,
                       const ZSetMemberVisitor& visitor) const;
  void VisitScoreRangeReversed(const RangeByScoreSpec* spec,
                               const ZSetMemberVisitor& visitor) const;
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
  containers::Listpack listpack_;
};
}  // namespace redis_simple::data_types

#endif  // REDIS_SIMPLE_DATA_TYPES_ZSET_ZSET_LISTPACK_H_
