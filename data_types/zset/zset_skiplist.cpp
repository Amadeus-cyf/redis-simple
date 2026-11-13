#include "data_types/zset/zset_skiplist.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::data_types {
ZSetSkiplist::ZSetSkiplist(size_t capacity)
    : dict_(containers::Dict<std::string, double>::Create(capacity)),
      skiplist_(std::make_unique<SkiplistType>()) {}

bool ZSetSkiplist::InsertOrUpdate(std::string_view member, double score,
                                  bool* const changed) {
  if (changed != nullptr) {
    *changed = false;
  }
  if (std::isnan(score)) {
    return false;
  }
  auto [current_score, inserted] = dict_->FindOrInsert(member);
  if (!inserted) {
    if (*current_score == score) {
      return false;
    }
    // Change the score in place; the skiplist repositions the entry if its
    // order changed, without copying the member name.
    skiplist_->Update(MemberRef{.score = *current_score, .member = member},
                      [score](ZSetEntry& entry) { entry.score = score; });
  } else {
    skiplist_->Insert(ZSetEntry(member, score));
  }
  *current_score = score;
  if (changed != nullptr) {
    *changed = true;
  }
  return inserted;
}

bool ZSetSkiplist::Delete(std::string_view member) {
  const double* score = dict_->FindValue(member);
  if (score == nullptr) {
    return false;
  }
  skiplist_->Delete(MemberRef{.score = *score, .member = member});
  return dict_->Delete(member);
}

std::optional<size_t> ZSetSkiplist::Rank(std::string_view member) const {
  const double* score = dict_->FindValue(member);
  if (score == nullptr) {
    return std::nullopt;
  }
  return skiplist_->FindRankOfKey(MemberRef{.score = *score, .member = member});
}

void ZSetSkiplist::ForEachInRankRange(const RangeByRankSpec* spec,
                                      const ZSetRangeVisitor& visitor) const {
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

void ZSetSkiplist::ForEachInScoreRange(const RangeByScoreSpec* spec,
                                       const ZSetRangeVisitor& visitor) const {
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
         spec->AboveMin((*skiplist_->Last()).member) &&
         spec->BelowMax((*skiplist_->Begin()).member);
}

void ZSetSkiplist::ForEachInLexRange(const RangeByLexSpec* spec,
                                     const ZSetRangeVisitor& visitor) const {
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
    if (!visitor((*entry).member, (*entry).score)) {
      return false;
    }
  }
  return true;
}

size_t ZSetSkiplist::Scan(size_t cursor, size_t count,
                          const ZSetScanVisitor& visitor) {
  return dict_->ScanStep(cursor, count,
                         [&visitor](const std::string& member, double score) {
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
