#include "data_types/zset/zset_skiplist.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::zset {
ZSetSkiplist::ZSetSkiplist()
    : dict_(in_memory::Dict<std::string, double>::Create()),
      skiplist_(std::make_unique<SkiplistType>(in_memory::kInitSkiplistLevel,
                                               Comparator(), Destructor())) {}

bool ZSetSkiplist::InsertOrUpdate(std::string_view key, double score) {
  if (std::isnan(score)) {
    return false;
  }
  const auto* current_score = dict_->FindValue(key);
  if (current_score != nullptr && *current_score == score) {
    // If the key exists and there is no change in score, do nothing.
    return false;
  }
  auto ze = std::make_unique<ZSetEntry>(key, score);
  bool inserted = false;
  if (current_score != nullptr) {
    // Update the score.
    const ZSetEntry old(key, *current_score);
    if (!skiplist_->Update(&old, ze.get())) {
      return false;
    }
  } else {
    // Insert a new key.
    const auto* inserted_entry = skiplist_->Insert(ze.get());
    if (inserted_entry == nullptr) {
      return false;
    }
    inserted = true;
  }
  [[maybe_unused]] auto* skiplist_owned_entry = ze.release();
  *dict_->FindOrInsert(key).first = score;
  // Update min and max key.
  if (!min_key_.has_value() || key < std::string_view(*min_key_)) {
    min_key_.emplace(key.data(), key.size());
  }
  if (!max_key_.has_value() || key > std::string_view(*max_key_)) {
    max_key_.emplace(key.data(), key.size());
  }
  return inserted;
}

bool ZSetSkiplist::Delete(std::string_view key) {
  const auto* score = dict_->FindValue(key);
  if (score == nullptr) {
    return false;
  }
  const ZSetEntry ze(key, *score);
  if (!skiplist_->Delete(&ze)) {
    return false;
  }
  if (!dict_->Delete(key)) {
    return false;
  }
  if (((min_key_.has_value() && std::string_view(*min_key_) == key) ||
       (max_key_.has_value() && std::string_view(*max_key_) == key))) {
    RecomputeMinMaxKeys();
  }
  return true;
}

std::optional<size_t> ZSetSkiplist::Rank(std::string_view key) const {
  const auto* score = dict_->FindValue(key);
  if (score == nullptr) {
    return std::nullopt;
  }
  const ZSetEntry ze(key, *score);
  return skiplist_->FindRankOfKey(&ze);
}

ZSetEntryList ZSetSkiplist::RangeByRank(const RangeByRankSpec* spec) const {
  if (spec == nullptr) {
    return {};
  }
  const auto skiplist_spec = ToSkiplistRangeByRankSpec(spec);
  if (skiplist_spec == nullptr) {
    return {};
  }
  return spec->reverse ? skiplist_->RevRangeByRank(skiplist_spec.get())
                       : skiplist_->RangeByRank(skiplist_spec.get());
}

ZSetEntryList ZSetSkiplist::RangeByScore(const RangeByScoreSpec* spec) const {
  if (spec == nullptr || Size() == 0 || spec->min > spec->max ||
      (spec->min == spec->max && (spec->minex || spec->maxex))) {
    return {};
  }
  const auto skiplist_spec = ToSkiplistRangeByKeySpec(spec);
  return spec->reverse ? skiplist_->RevRangeByKey(skiplist_spec.get())
                       : skiplist_->RangeByKey(skiplist_spec.get());
}

size_t ZSetSkiplist::Count(const RangeByScoreSpec* spec) const {
  if (spec == nullptr || Size() == 0 || spec->min > spec->max ||
      (spec->min == spec->max && (spec->minex || spec->maxex))) {
    return 0;
  }
  const auto skiplist_spec = ToSkiplistRangeByKeySpec(spec);
  return skiplist_->Count(skiplist_spec.get());
}

bool ZSetSkiplist::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  for (auto entry = skiplist_->Begin(); entry != skiplist_->End(); ++entry) {
    const auto* value = *entry;
    if (value == nullptr || !visitor(value->key, value->score)) {
      return false;
    }
  }
  return true;
}

ZSetSkiplist::RankSpecPtr ZSetSkiplist::ToSkiplistRangeByRankSpec(
    const RangeByRankSpec* spec) const {
  if (spec == nullptr) {
    return {nullptr};
  }
  const auto normalized = NormalizeRankRange(*spec, Size());
  if (!normalized.has_value()) {
    return {nullptr};
  }
  auto skiplist_spec = RankSpecPtr(new SkiplistRangeByRankSpec());
  skiplist_spec->min = static_cast<size_t>(normalized->min);
  skiplist_spec->max = static_cast<size_t>(normalized->max);
  skiplist_spec->minex = normalized->minex;
  skiplist_spec->maxex = normalized->maxex;
  if (normalized->limit) {
    auto limit = std::make_unique<SkiplistLimitSpec>();
    limit->offset = normalized->limit->offset;
    limit->count = normalized->limit->count;
    skiplist_spec->limit = limit.release();
  }
  return skiplist_spec;
}

ZSetSkiplist::KeySpecPtr ZSetSkiplist::ToSkiplistRangeByKeySpec(
    const RangeByScoreSpec* spec) const {
  if ((spec == nullptr) || !min_key_.has_value() || !max_key_.has_value()) {
    return {nullptr};
  }
  std::unique_ptr<SkiplistLimitSpec> limit;
  if (spec->limit) {
    limit = std::make_unique<SkiplistLimitSpec>();
    limit->offset = spec->limit->offset;
    limit->count = spec->limit->count;
  }

  // If min score exclusive, set the zset entry key to be the max_key to exclude
  // all keys with the same score.
  auto min_entry = std::make_unique<ZSetEntry>(
      spec->minex ? *max_key_ : *min_key_, spec->min);
  // If max score exclusive, set the zset entry key to be the min_key to exclude
  // all keys with the same score.
  auto max_entry = std::make_unique<ZSetEntry>(
      spec->maxex ? *min_key_ : *max_key_, spec->max);
  auto skiplist_spec = KeySpecPtr(new SkiplistRangeByKeySpec(
      min_entry.release(), spec->minex, max_entry.release(), spec->maxex,
      limit.release()));
  return skiplist_spec;
}

void ZSetSkiplist::RecomputeMinMaxKeys() {
  min_key_.reset();
  max_key_.reset();
  auto it = in_memory::Dict<std::string, double>::Iterator(dict_.get());
  it.SeekToFirst();
  while (it.Valid()) {
    const std::string& key = it.Key();
    if (!min_key_.has_value() || key < *min_key_) {
      min_key_.emplace(key);
    }
    if (!max_key_.has_value() || key > *max_key_) {
      max_key_.emplace(key);
    }
    it.Next();
  }
}

void ZSetSkiplist::RankSpecDeleter::operator()(
    SkiplistRangeByRankSpec* spec) const {
  if (spec != nullptr) {
    delete spec->limit;
    delete spec;
  }
}

void ZSetSkiplist::KeySpecDeleter::operator()(
    SkiplistRangeByKeySpec* spec) const {
  if (spec != nullptr) {
    delete spec->limit;
    delete spec->min;
    delete spec->max;
    delete spec;
  }
}
}  // namespace redis_simple::zset
