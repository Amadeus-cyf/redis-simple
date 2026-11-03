#include "data_types/zset/zset_listpack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "memory/listpack.h"
#include "utils/float_utils.h"

namespace redis_simple::zset {
ZSetListPack::ZSetListPack()
    : listpack_(std::make_unique<in_memory::ListPack>()) {}

bool ZSetListPack::InsertOrUpdate(std::string_view key, double score) {
  if (std::isnan(score)) {
    return false;
  }
  const auto key_idx = listpack_->FindAndSkip(key, 1);
  const bool inserted = !key_idx.has_value();
  if (key_idx.has_value()) {
    const auto entry = EntryAt(*key_idx);
    if (!entry.has_value()) {
      return false;
    }
    if (entry->score == score) {
      return false;
    }
    DeleteKeyScorePair(*key_idx);
  }
  // Like Redis, the listpack encoding stores a whole-number score as an
  // integer, which drops the sign of negative zero. The skiplist keeps it.
  const std::string score_str = utils::FloatToString(score == 0 ? 0.0 : score);
  auto idx = listpack_->First();
  while (idx.has_value()) {
    const auto entry = EntryAt(*idx);
    if (!entry.has_value()) {
      break;
    }
    if (score < entry->score || (score == entry->score && key < entry->key)) {
      if (!listpack_->Insert(*idx, key)) {
        return false;
      }
      const auto score_idx = listpack_->Next(*idx);
      if (!score_idx.has_value() || !listpack_->Insert(*score_idx, score_str)) {
        listpack_->Delete(*idx);
        return false;
      }
      return inserted;
    }
    idx = NextKeyAfterScore(entry->score_index);
  }
  if (!listpack_->Append(key)) {
    return false;
  }
  const auto appended_key = listpack_->Last();
  if (!listpack_->Append(score_str)) {
    if (appended_key.has_value()) {
      listpack_->Delete(*appended_key);
    }
    return false;
  }
  return inserted;
}

bool ZSetListPack::Delete(std::string_view key) {
  const auto idx = listpack_->FindAndSkip(key, 1);
  if (!idx.has_value()) {
    return false;
  }
  DeleteKeyScorePair(*idx);
  return true;
}

std::optional<double> ZSetListPack::Score(std::string_view key) const {
  const auto idx = listpack_->FindAndSkip(key, 1);
  if (!idx.has_value()) {
    return std::nullopt;
  }
  const auto entry = EntryAt(*idx);
  return entry.has_value() ? std::optional<double>(entry->score) : std::nullopt;
}

std::optional<size_t> ZSetListPack::Rank(std::string_view key) const {
  const auto key_idx = listpack_->FindAndSkip(key, 1);
  if (!key_idx.has_value()) {
    return std::nullopt;
  }
  auto idx = listpack_->First();
  size_t rank = 0;
  while (idx.has_value() && idx != key_idx) {
    const auto entry = EntryAt(*idx);
    if (!entry.has_value()) {
      return std::nullopt;
    }
    idx = NextKeyAfterScore(entry->score_index);
    ++rank;
  }
  return idx == key_idx ? std::optional<size_t>(rank) : std::nullopt;
}

ZSetEntryList ZSetListPack::RangeByRank(const RangeByRankSpec* spec) const {
  range_cache_.clear();
  range_cache_.reserve(Size());
  if (spec == nullptr) {
    return {};
  }
  const auto rank_spec = NormalizeRankRange(*spec, Size());
  if (!rank_spec.has_value() || !ValidateRangeRankSpec(&*rank_spec)) {
    return {};
  }
  return spec->reverse ? RevRangeByRankUtil(&*rank_spec)
                       : RangeByRankUtil(&*rank_spec);
}

ZSetEntryList ZSetListPack::RangeByScore(const RangeByScoreSpec* spec) const {
  range_cache_.clear();
  range_cache_.reserve(Size());
  if (!ValidateRangeScoreSpec(spec)) {
    return {};
  }
  return spec->reverse ? RevRangeByScoreUtil(spec) : RangeByScoreUtil(spec);
}

size_t ZSetListPack::Count(const RangeByScoreSpec* spec) const {
  if (!ValidateRangeScoreSpec(spec)) {
    return 0;
  }
  auto idx = listpack_->First();
  size_t count = 0;
  while (idx.has_value()) {
    const auto entry = EntryAt(*idx);
    if (!entry.has_value()) {
      break;
    }
    if (IsInRange(entry->score, spec)) {
      ++count;
    } else if (!LessOrEqual(entry->score, spec)) {
      break;
    }
    idx = NextKeyAfterScore(entry->score_index);
  }
  return count;
}

bool ZSetListPack::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  auto key_idx = listpack_->First();
  while (key_idx.has_value()) {
    const auto entry = EntryAt(*key_idx);
    if (!entry.has_value()) {
      return false;
    }
    if (!visitor(entry->key, entry->score)) {
      return false;
    }
    key_idx = NextKeyAfterScore(entry->score_index);
  }
  return true;
}

void ZSetListPack::DeleteKeyScorePair(size_t idx) {
  listpack_->DeleteRange(idx, 2);
}

std::optional<std::string_view> ZSetListPack::ValueAt(size_t idx) const {
  size_t len = 0;
  const auto* data = listpack_->Get(idx, &len);
  if (data == nullptr) {
    return std::nullopt;
  }
  return std::string_view(reinterpret_cast<const char*>(data), len);
}

std::optional<ZSetListPack::EntryView> ZSetListPack::EntryAt(
    size_t key_idx) const {
  const auto score_idx = listpack_->Next(key_idx);
  if (!score_idx.has_value()) {
    return std::nullopt;
  }
  const auto score = ScoreAt(*score_idx);
  if (!score.has_value()) {
    return std::nullopt;
  }
  // Read the key last because numeric values share ListPack's conversion
  // buffer. Reading a numeric score after the key would invalidate its view.
  const auto key = ValueAt(key_idx);
  if (!key.has_value()) {
    return std::nullopt;
  }
  return EntryView{*key, *score, key_idx, *score_idx};
}

std::optional<double> ZSetListPack::ScoreAt(size_t idx) const {
  // Integral scores are stored with integer encodings. Convert them directly
  // instead of rendering them as text and parsing that text back; the text
  // was formatted from a double, so both paths give the same score.
  if (const auto integer = listpack_->IntegerAt(idx); integer.has_value()) {
    return static_cast<double>(*integer);
  }
  const auto value = ValueAt(idx);
  double score = 0;
  if (!value.has_value() || !utils::ToDouble(*value, &score)) {
    return std::nullopt;
  }
  return score;
}

ZSetEntryList ZSetListPack::RangeByRankUtil(const RangeByRankSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return {};
  }
  auto idx = listpack_->First();
  ZSetEntryList keys;
  size_t rank = 0;
  const size_t size = Size();
  if (size == 0) {
    return {};
  }
  const auto min_rank = static_cast<size_t>(spec->min);
  const auto max_rank = static_cast<size_t>(spec->max);
  const size_t start = min_rank + (spec->minex ? 1 : 0);
  size_t end = std::min(max_rank, size);
  if (!spec->maxex && max_rank < size) {
    end = max_rank + 1;
  }
  const size_t offset = spec->limit ? spec->limit->offset : 0;
  if (start >= size) {
    return {};
  }
  while (idx.has_value() && rank < end) {
    const auto entry = EntryAt(*idx);
    if (!entry.has_value()) {
      break;
    }
    if (rank >= start) {
      if (rank - start >= offset) {
        keys.push_back(AddRangeResult(entry->key, entry->score));
        if (count.has_value() && keys.size() >= *count) {
          break;
        }
      }
    }
    idx = NextKeyAfterScore(entry->score_index);
    ++rank;
  }
  return keys;
}

ZSetEntryList ZSetListPack::RevRangeByRankUtil(
    const RangeByRankSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return {};
  }
  auto idx = listpack_->Last();
  ZSetEntryList keys;
  size_t rank = 0;
  const size_t size = Size();
  if (size == 0) {
    return {};
  }
  const auto min_rank = static_cast<size_t>(spec->min);
  const auto max_rank = static_cast<size_t>(spec->max);
  const size_t start = min_rank + (spec->minex ? 1 : 0);
  size_t end = std::min(max_rank, size);
  if (!spec->maxex && max_rank < size) {
    end = max_rank + 1;
  }
  const size_t offset = spec->limit ? spec->limit->offset : 0;
  if (start >= size) {
    return {};
  }
  while (idx.has_value() && rank < end) {
    const auto key_idx = listpack_->Prev(*idx);
    if (!key_idx.has_value()) {
      break;
    }
    const auto entry = EntryAt(*key_idx);
    if (!entry.has_value()) {
      break;
    }
    if (rank >= start) {
      if (rank - start >= offset) {
        keys.push_back(AddRangeResult(entry->key, entry->score));
        if (count.has_value() && keys.size() >= *count) {
          break;
        }
      }
    }
    idx = PrevScoreBeforeKey(entry->key_index);
    ++rank;
  }
  return keys;
}

ZSetEntryList ZSetListPack::RangeByScoreUtil(
    const RangeByScoreSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return {};
  }
  auto key_idx = FindKeyGreaterOrEqual(spec);
  if (!key_idx.has_value()) {
    return {};
  }
  ZSetEntryList keys;
  size_t i = 0;
  size_t offset = spec->limit ? spec->limit->offset : 0;
  while (key_idx.has_value()) {
    const auto entry = EntryAt(*key_idx);
    if (!entry.has_value() || !LessOrEqual(entry->score, spec)) {
      break;
    }
    if (IsInRange(entry->score, spec) && i >= offset) {
      keys.push_back(AddRangeResult(entry->key, entry->score));
      if (count.has_value() && keys.size() == *count) {
        break;
      }
    }
    key_idx = NextKeyAfterScore(entry->score_index);
    ++i;
  }
  return keys;
}

ZSetEntryList ZSetListPack::RevRangeByScoreUtil(
    const RangeByScoreSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return {};
  }
  auto key_idx = FindKeyLessOrEqual(spec);
  if (!key_idx.has_value()) {
    return {};
  }
  ZSetEntryList keys;
  size_t i = 0;
  size_t offset = spec->limit ? spec->limit->offset : 0;
  while (key_idx.has_value()) {
    const auto entry = EntryAt(*key_idx);
    if (!entry.has_value() || !GreaterOrEqual(entry->score, spec)) {
      break;
    }
    if (IsInRange(entry->score, spec) && i >= offset) {
      keys.push_back(AddRangeResult(entry->key, entry->score));
      if (count.has_value() && keys.size() == *count) {
        break;
      }
    }
    key_idx = PrevKeyBeforeKey(entry->key_index);
    ++i;
  }
  return keys;
}

const ZSetEntry* ZSetListPack::AddRangeResult(std::string_view key,
                                              double score) const {
  range_cache_.emplace_back(key, score);
  return &range_cache_.back();
}

std::optional<size_t> ZSetListPack::NextKeyAfterScore(size_t score_idx) const {
  return listpack_->Next(score_idx);
}

std::optional<size_t> ZSetListPack::PrevScoreBeforeKey(size_t key_idx) const {
  return listpack_->Prev(key_idx);
}

std::optional<size_t> ZSetListPack::PrevKeyBeforeKey(size_t key_idx) const {
  const auto score_idx = PrevScoreBeforeKey(key_idx);
  return score_idx.has_value() ? listpack_->Prev(*score_idx) : std::nullopt;
}

std::optional<size_t> ZSetListPack::FindKeyGreaterOrEqual(
    const RangeByScoreSpec* spec) const {
  auto key_idx = listpack_->First();
  while (key_idx.has_value()) {
    const auto entry = EntryAt(*key_idx);
    if (!entry.has_value()) {
      break;
    }
    if (GreaterOrEqual(entry->score, spec)) {
      return key_idx;
    }
    key_idx = NextKeyAfterScore(entry->score_index);
  }
  return std::nullopt;
}

std::optional<size_t> ZSetListPack::FindKeyLessOrEqual(
    const RangeByScoreSpec* spec) const {
  const auto score_idx = listpack_->Last();
  if (!score_idx.has_value()) {
    return std::nullopt;
  }
  auto key_idx = listpack_->Prev(*score_idx);
  while (key_idx.has_value()) {
    const auto entry = EntryAt(*key_idx);
    if (!entry.has_value()) {
      break;
    }
    if (LessOrEqual(entry->score, spec)) {
      return key_idx;
    }
    key_idx = PrevKeyBeforeKey(entry->key_index);
  }
  return std::nullopt;
}

bool ZSetListPack::ValidateRangeRankSpec(const RangeByRankSpec* spec) {
  return (spec != nullptr) && spec->min >= 0 && spec->max >= 0 &&
         ((!spec->minex && !spec->maxex && spec->min <= spec->max) ||
          spec->min < spec->max);
}

bool ZSetListPack::ValidateRangeScoreSpec(const RangeByScoreSpec* spec) {
  return (spec != nullptr) &&
         ((!spec->minex && !spec->maxex && spec->min <= spec->max) ||
          spec->min < spec->max);
}

bool ZSetListPack::IsInRange(double score, const RangeByScoreSpec* spec) {
  return (spec->minex ? score > spec->min : score >= spec->min) &&
         (spec->maxex ? score < spec->max : score <= spec->max);
}

bool ZSetListPack::GreaterOrEqual(double score, const RangeByScoreSpec* spec) {
  return spec->minex ? score > spec->min : score >= spec->min;
}

bool ZSetListPack::LessOrEqual(double score, const RangeByScoreSpec* spec) {
  return spec->maxex ? score < spec->max : score <= spec->max;
}
}  // namespace redis_simple::zset
