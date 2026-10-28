#include "data_types/zset/zset_skiplist.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::zset {
ZSetSkiplist::ZSetSkiplist()
    : dict_(in_memory::Dict<std::string, double>::Create()),
      skiplist_(std::make_unique<SkiplistType>()) {}

bool ZSetSkiplist::InsertOrUpdate(std::string_view key, double score) {
  if (std::isnan(score)) {
    return false;
  }
  auto [current_score, inserted] = dict_->FindOrInsert(key);
  if (!inserted) {
    if (*current_score == score) {
      return false;
    }
    // Change the score in place; the skiplist repositions the entry if its
    // order changed, without copying the member name.
    skiplist_->Update(MemberRef{.score = *current_score, .key = key},
                      [score](ZSetEntry& entry) { entry.score = score; });
  } else {
    skiplist_->Insert(ZSetEntry(key, score));
  }
  *current_score = score;
  return inserted;
}

bool ZSetSkiplist::Delete(std::string_view key) {
  const auto* score = dict_->FindValue(key);
  if (score == nullptr) {
    return false;
  }
  skiplist_->Delete(MemberRef{.score = *score, .key = key});
  return dict_->Delete(key);
}

std::optional<size_t> ZSetSkiplist::Rank(std::string_view key) const {
  const auto* score = dict_->FindValue(key);
  if (score == nullptr) {
    return std::nullopt;
  }
  return skiplist_->FindRankOfKey(MemberRef{.score = *score, .key = key});
}

ZSetEntryList ZSetSkiplist::RangeByRank(const RangeByRankSpec* spec) const {
  if (spec == nullptr) {
    return {};
  }
  const auto normalized = NormalizeRankRange(*spec, Size());
  if (!normalized.has_value()) {
    return {};
  }
  const SkiplistType::RankRange range{
      .min = static_cast<size_t>(normalized->min),
      .max = static_cast<size_t>(normalized->max),
      .min_exclusive = normalized->minex,
      .max_exclusive = normalized->maxex,
      .limit = ToLimit(normalized->limit),
      .reverse = spec->reverse,
  };
  ZSetEntryList entries;
  skiplist_->ForEachInRange(
      range, [&entries](const ZSetEntry& entry) { entries.push_back(&entry); });
  return entries;
}

ZSetEntryList ZSetSkiplist::RangeByScore(const RangeByScoreSpec* spec) const {
  if (spec == nullptr) {
    return {};
  }
  const SkiplistType::KeyRange<double> range{
      .min = spec->min,
      .max = spec->max,
      .min_exclusive = spec->minex,
      .max_exclusive = spec->maxex,
      .limit = ToLimit(spec->limit),
      .reverse = spec->reverse,
  };
  ZSetEntryList entries;
  skiplist_->ForEachInRange(
      range, [&entries](const ZSetEntry& entry) { entries.push_back(&entry); });
  return entries;
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

bool ZSetSkiplist::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  for (auto entry = skiplist_->Begin(); entry != skiplist_->End(); ++entry) {
    if (!visitor((*entry).key, (*entry).score)) {
      return false;
    }
  }
  return true;
}

std::optional<ZSetSkiplist::SkiplistType::Limit> ZSetSkiplist::ToLimit(
    const std::optional<LimitSpec>& limit) {
  if (!limit.has_value()) {
    return std::nullopt;
  }
  return SkiplistType::Limit{.offset = limit->offset, .count = limit->count};
}
}  // namespace redis_simple::zset
