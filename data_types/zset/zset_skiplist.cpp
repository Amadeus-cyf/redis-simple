#include "data_types/zset/zset_skiplist.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::data_types {
ZSetSkiplist::ZSetSkiplist(size_t capacity)
    : dict_(zset_skiplist_internal::MemberDict::Create(capacity)),
      skiplist_(std::make_unique<SkiplistType>()) {}

ZSetAddResult ZSetSkiplist::Add(std::string_view member, double score,
                                const ZSetAddRules& rules) {
  if (std::isnan(score)) {
    return {.status = ZSetAddStatus::kNotANumber, .score = score};
  }
  if (rules.xx) {
    // XX never adds, so a plain find is the one lookup.
    double* const current = dict_->FindValue(member);
    if (current == nullptr) {
      return {.status = ZSetAddStatus::kSkipped, .score = score};
    }
    return UpdateExisting(member, current, score, rules);
  }
  const auto [current, stored_member, inserted] =
      dict_->FindOrInsertEntry(member);
  if (!inserted) {
    return UpdateExisting(member, current, score, rules);
  }
  skiplist_->Insert(ScoredEntry{.score = score, .member = stored_member});
  *current = score;
  return {.status = ZSetAddStatus::kAdded, .score = score};
}

ZSetAddResult ZSetSkiplist::UpdateExisting(std::string_view member,
                                           double* const current, double score,
                                           const ZSetAddRules& rules) {
  const ZSetAddResult result = ApplyAddRulesToExisting(*current, score, rules);
  if (result.status == ZSetAddStatus::kUpdated) {
    // Change the score in place; the skiplist repositions the entry if its
    // order changed.
    skiplist_->Update(ScoredMember{.score = *current, .member = member},
                      [updated = result.score](ScoredEntry& entry) {
                        entry.score = updated;
                      });
    *current = result.score;
  }
  return result;
}

bool ZSetSkiplist::Delete(std::string_view member) {
  const double* score = dict_->FindValue(member);
  if (score == nullptr) {
    return false;
  }
  // The skiplist entry goes first: it views the dict entry's name.
  skiplist_->Delete(ScoredMember{.score = *score, .member = member});
  return dict_->Delete(member);
}

std::optional<ZSetRank> ZSetSkiplist::RankAndScore(
    std::string_view member) const {
  const double* score = dict_->FindValue(member);
  if (score == nullptr) {
    return std::nullopt;
  }
  const std::optional<size_t> rank =
      skiplist_->FindRankOfKey(ScoredMember{.score = *score, .member = member});
  if (!rank.has_value()) {
    return std::nullopt;
  }
  return ZSetRank{.rank = *rank, .score = *score};
}

void ZSetSkiplist::ForEachInRankRange(const RangeByRankSpec* spec,
                                      const ZSetMemberVisitor& visitor) const {
  VisitRankRange(spec, visitor);
}

void ZSetSkiplist::ForEachInScoreRange(const RangeByScoreSpec* spec,
                                       const ZSetMemberVisitor& visitor) const {
  VisitScoreRange(spec, visitor);
}

size_t ZSetSkiplist::Count(const RangeByScoreSpec* spec) const {
  if (spec == nullptr) {
    return 0;
  }
  return skiplist_->Count(SkiplistType::KeyRange<double>{
      .min = spec->min,
      .max = spec->max,
      .min_exclusive = spec->minex,
      .max_exclusive = spec->maxex,
      .limit = std::nullopt,
      .reverse = false,
  });
}

bool ZSetSkiplist::OverlapsLexRange(const RangeByLexSpec* spec) const {
  // Redis' zslIsInLexRange: the last member reaches the minimum and the
  // first member the maximum.
  return spec != nullptr && !spec->Empty() && skiplist_->Size() > 0 &&
         spec->AboveMin((*skiplist_->Last()).Member()) &&
         spec->BelowMax((*skiplist_->Begin()).Member());
}

void ZSetSkiplist::ForEachInLexRange(const RangeByLexSpec* spec,
                                     const ZSetMemberVisitor& visitor) const {
  VisitLexRange(spec, visitor);
}

size_t ZSetSkiplist::LexCount(const RangeByLexSpec* spec) const {
  if (!OverlapsLexRange(spec)) {
    return 0;
  }
  return skiplist_->Count(SkiplistType::KeyRange<LexBound>{
      .min = spec->min,
      .max = spec->max,
      .min_exclusive = spec->minex,
      .max_exclusive = spec->maxex,
      .limit = std::nullopt,
      .reverse = false,
  });
}

bool ZSetSkiplist::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  for (auto entry = skiplist_->Begin(); entry != skiplist_->End(); ++entry) {
    if (!visitor((*entry).Member(), (*entry).score)) {
      return false;
    }
  }
  return true;
}

bool ZSetSkiplist::ForEachEntryDescending(
    const ZSetEntryVisitor& visitor) const {
  for (auto entry = skiplist_->Last(); entry != skiplist_->End(); --entry) {
    if (!visitor((*entry).Member(), (*entry).score)) {
      return false;
    }
  }
  return true;
}

size_t ZSetSkiplist::Scan(size_t cursor, size_t count,
                          const ZSetMemberVisitor& visitor) {
  return dict_->ScanStep(cursor, count,
                         [&visitor](std::string_view member, double score) {
                           visitor(member, score);
                         });
}

std::optional<ZSetSkiplist::SkiplistType::Limit> ZSetSkiplist::ToLimit(
    const std::optional<LimitSpec>& limit) {
  if (!limit.has_value()) {
    return std::nullopt;
  }
  return SkiplistType::Limit{.offset = limit->offset, .count = limit->count};
}
}  // namespace redis_simple::data_types
