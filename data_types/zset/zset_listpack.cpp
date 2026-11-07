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
    : listpack_(std::make_unique<memory::ListPack>()) {}

bool ZSetListPack::InsertOrUpdate(std::string_view member, double score,
                                  bool* const changed) {
  if (changed != nullptr) {
    *changed = false;
  }
  if (std::isnan(score)) {
    return false;
  }
  const std::optional<size_t> member_offset = listpack_->FindAndSkip(member, 1);
  const bool inserted = !member_offset.has_value();
  if (member_offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value()) {
      return false;
    }
    if (entry->score == score) {
      return false;
    }
    DeleteMemberScorePair(*member_offset);
  }
  // Like Redis, the listpack encoding stores a whole-number score as an
  // integer, which drops the sign of negative zero. The skiplist keeps it.
  const std::string score_text = utils::FloatToString(score == 0 ? 0.0 : score);
  std::optional<size_t> offset = listpack_->First();
  while (offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*offset);
    if (!entry.has_value()) {
      break;
    }
    if (score < entry->score ||
        (score == entry->score && member < entry->member)) {
      if (!listpack_->Insert(*offset, member)) {
        return false;
      }
      const std::optional<size_t> score_offset = listpack_->Next(*offset);
      if (!score_offset.has_value() ||
          !listpack_->Insert(*score_offset, score_text)) {
        listpack_->Delete(*offset);
        return false;
      }
      if (changed != nullptr) {
        *changed = true;
      }
      return inserted;
    }
    offset = NextMemberAfterScore(entry->score_index);
  }
  if (!listpack_->Append(member)) {
    return false;
  }
  const std::optional<size_t> appended_member = listpack_->Last();
  if (!listpack_->Append(score_text)) {
    if (appended_member.has_value()) {
      listpack_->Delete(*appended_member);
    }
    return false;
  }
  if (changed != nullptr) {
    *changed = true;
  }
  return inserted;
}

bool ZSetListPack::Delete(std::string_view member) {
  const std::optional<size_t> offset = listpack_->FindAndSkip(member, 1);
  if (!offset.has_value()) {
    return false;
  }
  DeleteMemberScorePair(*offset);
  return true;
}

std::optional<double> ZSetListPack::Score(std::string_view member) const {
  const std::optional<size_t> offset = listpack_->FindAndSkip(member, 1);
  if (!offset.has_value()) {
    return std::nullopt;
  }
  const std::optional<EntryView> entry = EntryAt(*offset);
  return entry.has_value() ? std::optional<double>(entry->score) : std::nullopt;
}

std::optional<size_t> ZSetListPack::Rank(std::string_view member) const {
  const std::optional<size_t> member_offset = listpack_->FindAndSkip(member, 1);
  if (!member_offset.has_value()) {
    return std::nullopt;
  }
  std::optional<size_t> offset = listpack_->First();
  size_t rank = 0;
  while (offset.has_value() && offset != member_offset) {
    const std::optional<EntryView> entry = EntryAt(*offset);
    if (!entry.has_value()) {
      return std::nullopt;
    }
    offset = NextMemberAfterScore(entry->score_index);
    ++rank;
  }
  return offset == member_offset ? std::optional<size_t>(rank) : std::nullopt;
}

ZSetEntryList ZSetListPack::RangeByRank(const RangeByRankSpec* spec) const {
  range_cache_.clear();
  range_cache_.reserve(Size());
  if (spec == nullptr) {
    return {};
  }
  const std::optional<RangeByRankSpec> rank_spec =
      NormalizeRankRange(*spec, Size());
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
  std::optional<size_t> offset = listpack_->First();
  size_t count = 0;
  while (offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*offset);
    if (!entry.has_value()) {
      break;
    }
    if (IsInRange(entry->score, spec)) {
      ++count;
    } else if (!LessOrEqual(entry->score, spec)) {
      break;
    }
    offset = NextMemberAfterScore(entry->score_index);
  }
  return count;
}

bool ZSetListPack::OverlapsLexRange(const RangeByLexSpec* spec) const {
  // Redis' zzlIsInLexRange: the last member reaches the minimum and the
  // first member the maximum.
  if (spec == nullptr || spec->Empty()) {
    return false;
  }
  const std::optional<size_t> first = listpack_->First();
  const std::optional<size_t> last_score = listpack_->Last();
  if (!first.has_value() || !last_score.has_value()) {
    return false;
  }
  const std::optional<size_t> last = listpack_->Prev(*last_score);
  // Check one member at a time: numeric members share a conversion buffer.
  const std::optional<std::string_view> last_member =
      last.has_value() ? ValueAt(*last) : std::nullopt;
  if (!last_member.has_value() || !spec->AboveMin(*last_member)) {
    return false;
  }
  const std::optional<std::string_view> first_member = ValueAt(*first);
  return first_member.has_value() && spec->BelowMax(*first_member);
}

ZSetEntryList ZSetListPack::RangeByLex(const RangeByLexSpec* spec) const {
  // Redis' zzlFirstInLexRange and zzlLastInLexRange: walk from one end to
  // the first member inside the range, then on while members stay in it.
  range_cache_.clear();
  range_cache_.reserve(Size());
  if (!OverlapsLexRange(spec)) {
    return {};
  }
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  size_t skip = spec->limit ? spec->limit->offset : 0;
  ZSetEntryList members;
  if (count.has_value() && *count == 0) {
    return members;
  }
  const auto visit = [&](const EntryView& entry) {
    if (skip > 0) {
      --skip;
      return true;
    }
    members.push_back(AddRangeResult(entry.member, entry.score));
    return !count.has_value() || members.size() < *count;
  };
  if (!spec->reverse) {
    bool started = false;
    std::optional<size_t> member_offset = listpack_->First();
    while (member_offset.has_value()) {
      const std::optional<EntryView> entry = EntryAt(*member_offset);
      if (!entry.has_value()) {
        break;
      }
      started = started || spec->AboveMin(entry->member);
      if (started && (!spec->BelowMax(entry->member) || !visit(*entry))) {
        break;
      }
      member_offset = NextMemberAfterScore(entry->score_index);
    }
    return members;
  }
  const std::optional<size_t> last_score = listpack_->Last();
  std::optional<size_t> member_offset =
      last_score.has_value() ? listpack_->Prev(*last_score) : std::nullopt;
  bool started = false;
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value()) {
      break;
    }
    started = started || spec->BelowMax(entry->member);
    if (started && (!spec->AboveMin(entry->member) || !visit(*entry))) {
      break;
    }
    member_offset = PrevMemberBeforeMember(entry->member_index);
  }
  return members;
}

size_t ZSetListPack::LexCount(const RangeByLexSpec* spec) const {
  if (!OverlapsLexRange(spec)) {
    return 0;
  }
  size_t count = 0;
  bool started = false;
  std::optional<size_t> member_offset = listpack_->First();
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value()) {
      break;
    }
    started = started || spec->AboveMin(entry->member);
    if (started) {
      if (!spec->BelowMax(entry->member)) {
        break;
      }
      ++count;
    }
    member_offset = NextMemberAfterScore(entry->score_index);
  }
  return count;
}

bool ZSetListPack::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  std::optional<size_t> member_offset = listpack_->First();
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value()) {
      return false;
    }
    if (!visitor(entry->member, entry->score)) {
      return false;
    }
    member_offset = NextMemberAfterScore(entry->score_index);
  }
  return true;
}

void ZSetListPack::DeleteMemberScorePair(size_t offset) {
  listpack_->DeleteRange(offset, 2);
}

std::optional<std::string_view> ZSetListPack::ValueAt(size_t offset) const {
  size_t length = 0;
  const unsigned char* data = listpack_->Get(offset, &length);
  if (data == nullptr) {
    return std::nullopt;
  }
  return std::string_view(reinterpret_cast<const char*>(data), length);
}

std::optional<ZSetListPack::EntryView> ZSetListPack::EntryAt(
    size_t member_offset) const {
  const std::optional<size_t> score_offset = listpack_->Next(member_offset);
  if (!score_offset.has_value()) {
    return std::nullopt;
  }
  const std::optional<double> score = ScoreAt(*score_offset);
  if (!score.has_value()) {
    return std::nullopt;
  }
  // Read the member last because numeric values share ListPack's conversion
  // buffer. Reading a numeric score after the member would invalidate its view.
  const std::optional<std::string_view> member = ValueAt(member_offset);
  if (!member.has_value()) {
    return std::nullopt;
  }
  return EntryView{*member, *score, member_offset, *score_offset};
}

std::optional<double> ZSetListPack::ScoreAt(size_t offset) const {
  // Integral scores are stored with integer encodings. Convert them directly
  // instead of rendering them as text and parsing that text back; the text
  // was formatted from a double, so both paths give the same score.
  if (const std::optional<int64_t> integer = listpack_->IntegerAt(offset);
      integer.has_value()) {
    return static_cast<double>(*integer);
  }
  const std::optional<std::string_view> value = ValueAt(offset);
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
  std::optional<size_t> offset = listpack_->First();
  ZSetEntryList members;
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
  const size_t limit_offset = spec->limit ? spec->limit->offset : 0;
  if (start >= size) {
    return {};
  }
  while (offset.has_value() && rank < end) {
    const std::optional<EntryView> entry = EntryAt(*offset);
    if (!entry.has_value()) {
      break;
    }
    if (rank >= start) {
      if (rank - start >= limit_offset) {
        members.push_back(AddRangeResult(entry->member, entry->score));
        if (count.has_value() && members.size() >= *count) {
          break;
        }
      }
    }
    offset = NextMemberAfterScore(entry->score_index);
    ++rank;
  }
  return members;
}

ZSetEntryList ZSetListPack::RevRangeByRankUtil(
    const RangeByRankSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return {};
  }
  std::optional<size_t> offset = listpack_->Last();
  ZSetEntryList members;
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
  const size_t limit_offset = spec->limit ? spec->limit->offset : 0;
  if (start >= size) {
    return {};
  }
  while (offset.has_value() && rank < end) {
    const std::optional<size_t> member_offset = listpack_->Prev(*offset);
    if (!member_offset.has_value()) {
      break;
    }
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value()) {
      break;
    }
    if (rank >= start) {
      if (rank - start >= limit_offset) {
        members.push_back(AddRangeResult(entry->member, entry->score));
        if (count.has_value() && members.size() >= *count) {
          break;
        }
      }
    }
    offset = PrevScoreBeforeMember(entry->member_index);
    ++rank;
  }
  return members;
}

ZSetEntryList ZSetListPack::RangeByScoreUtil(
    const RangeByScoreSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return {};
  }
  std::optional<size_t> member_offset = FindKeyGreaterOrEqual(spec);
  if (!member_offset.has_value()) {
    return {};
  }
  ZSetEntryList members;
  size_t i = 0;
  size_t offset = spec->limit ? spec->limit->offset : 0;
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value() || !LessOrEqual(entry->score, spec)) {
      break;
    }
    if (IsInRange(entry->score, spec) && i >= offset) {
      members.push_back(AddRangeResult(entry->member, entry->score));
      if (count.has_value() && members.size() == *count) {
        break;
      }
    }
    member_offset = NextMemberAfterScore(entry->score_index);
    ++i;
  }
  return members;
}

ZSetEntryList ZSetListPack::RevRangeByScoreUtil(
    const RangeByScoreSpec* spec) const {
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return {};
  }
  std::optional<size_t> member_offset = FindKeyLessOrEqual(spec);
  if (!member_offset.has_value()) {
    return {};
  }
  ZSetEntryList members;
  size_t i = 0;
  size_t offset = spec->limit ? spec->limit->offset : 0;
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value() || !GreaterOrEqual(entry->score, spec)) {
      break;
    }
    if (IsInRange(entry->score, spec) && i >= offset) {
      members.push_back(AddRangeResult(entry->member, entry->score));
      if (count.has_value() && members.size() == *count) {
        break;
      }
    }
    member_offset = PrevMemberBeforeMember(entry->member_index);
    ++i;
  }
  return members;
}

const ZSetEntry* ZSetListPack::AddRangeResult(std::string_view member,
                                              double score) const {
  range_cache_.emplace_back(member, score);
  return &range_cache_.back();
}

std::optional<size_t> ZSetListPack::NextMemberAfterScore(
    size_t score_offset) const {
  return listpack_->Next(score_offset);
}

std::optional<size_t> ZSetListPack::PrevScoreBeforeMember(
    size_t member_offset) const {
  return listpack_->Prev(member_offset);
}

std::optional<size_t> ZSetListPack::PrevMemberBeforeMember(
    size_t member_offset) const {
  const std::optional<size_t> score_offset =
      PrevScoreBeforeMember(member_offset);
  return score_offset.has_value() ? listpack_->Prev(*score_offset)
                                  : std::nullopt;
}

std::optional<size_t> ZSetListPack::FindKeyGreaterOrEqual(
    const RangeByScoreSpec* spec) const {
  std::optional<size_t> member_offset = listpack_->First();
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value()) {
      break;
    }
    if (GreaterOrEqual(entry->score, spec)) {
      return member_offset;
    }
    member_offset = NextMemberAfterScore(entry->score_index);
  }
  return std::nullopt;
}

std::optional<size_t> ZSetListPack::FindKeyLessOrEqual(
    const RangeByScoreSpec* spec) const {
  const std::optional<size_t> score_offset = listpack_->Last();
  if (!score_offset.has_value()) {
    return std::nullopt;
  }
  std::optional<size_t> member_offset = listpack_->Prev(*score_offset);
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*member_offset);
    if (!entry.has_value()) {
      break;
    }
    if (LessOrEqual(entry->score, spec)) {
      return member_offset;
    }
    member_offset = PrevMemberBeforeMember(entry->member_index);
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
