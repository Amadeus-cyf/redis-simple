#include "data_types/zset/zset_listpack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "containers/listpack.h"
#include "utils/check.h"
#include "utils/floats.h"

namespace redis_simple::data_types {
namespace {
// The ranks a rank range visits: `first` up to, but not including, `end`.
struct VisitedRanks {
  size_t first;
  size_t end;
};

// The ranks that `spec`, a normalized and valid rank range, visits in a
// sorted set of `size` members, with its exclusive ends and its limit
// applied. Nullopt when it visits none.
std::optional<VisitedRanks> RanksToVisit(const RangeByRankSpec& spec,
                                         size_t size) {
  const auto min_rank = static_cast<size_t>(spec.min);
  const auto max_rank = static_cast<size_t>(spec.max);
  const size_t start = min_rank + (spec.minex ? 1 : 0);
  size_t end = std::min(max_rank, size);
  if (!spec.maxex && max_rank < size) {
    end = max_rank + 1;
  }
  const size_t limit_offset = spec.limit ? spec.limit->offset : 0;
  const std::optional<size_t> limit_count =
      spec.limit ? spec.limit->count : std::nullopt;
  // The offset and the count are compared with the ranks that are left,
  // since either may be too large to add to a rank.
  if (start >= end || limit_offset >= end - start) {
    return std::nullopt;
  }
  const size_t first = start + limit_offset;
  if (limit_count.has_value()) {
    end = first + std::min(*limit_count, end - first);
  }
  if (first >= end) {
    return std::nullopt;
  }
  return VisitedRanks{.first = first, .end = end};
}
}  // namespace

ZSetListpack::ZSetListpack() = default;

ZSetAddResult ZSetListpack::Add(std::string_view member, double score,
                                const ZSetAddRules& rules) {
  if (std::isnan(score)) {
    return {.status = ZSetAddStatus::kNotANumber, .score = score};
  }
  const ZSetAddResult skipped{.status = ZSetAddStatus::kSkipped,
                              .score = score};
  const std::optional<size_t> member_offset = listpack_.FindAndSkip(member, 1);
  if (!member_offset.has_value()) {
    if (rules.xx || !InsertSorted(member, score)) {
      return skipped;
    }
    return {.status = ZSetAddStatus::kAdded, .score = score};
  }
  containers::Listpack::IntBuffer member_buffer;
  const std::optional<EntryView> current =
      EntryAt(*member_offset, &member_buffer);
  if (!current.has_value()) {
    return skipped;
  }
  const ZSetAddResult result =
      ApplyAddRulesToExisting(current->score, score, rules);
  if (result.status != ZSetAddStatus::kUpdated) {
    return result;
  }
  // Like Redis, move the member by deleting its pair and inserting it again.
  // A listpack insert fails only past the 4 GiB a listpack can address, and
  // this one adds back a pair a few bytes longer than the one it replaces in
  // a sorted set small enough to be a listpack. Were it to fail, the member
  // would be lost, so stop instead.
  DeleteMemberScorePair(*member_offset);
  const bool inserted = InsertSorted(member, result.score);
  RS_CHECK(inserted);
  return result;
}

bool ZSetListpack::InsertSorted(std::string_view member, double score) {
  // Like Redis, the listpack encoding stores a whole-number score as an
  // integer, which drops the sign of negative zero. The skiplist keeps it.
  const std::string score_text = utils::FloatToString(score == 0 ? 0.0 : score);
  // Member and score go in as one listpack insert, as in Redis' zzlInsertAt,
  // so the bytes after them are moved once.
  const std::optional<size_t> successor = FirstMemberAfter(member, score);
  return successor.has_value()
             ? listpack_.BatchInsert(
                   *successor, {{member, 0, false}, {score_text, 0, false}})
             : listpack_.BatchAppend(
                   {{member, 0, false}, {score_text, 0, false}});
}

std::optional<size_t> ZSetListpack::FirstMemberAfter(std::string_view member,
                                                     double score) const {
  containers::Listpack::IntBuffer member_buffer;
  std::optional<size_t> offset = listpack_.First();
  while (offset.has_value()) {
    const std::optional<size_t> score_offset = listpack_.Next(*offset);
    if (!score_offset.has_value()) {
      break;
    }
    const std::optional<double> entry_score = ScoreAt(*score_offset);
    if (!entry_score.has_value()) {
      break;
    }
    if (score < *entry_score) {
      return offset;
    }
    // As in Redis' zzlInsert, a member is read only when the scores tie.
    if (score == *entry_score) {
      const std::optional<std::string_view> entry_member =
          ValueAt(*offset, &member_buffer);
      if (!entry_member.has_value()) {
        break;
      }
      if (member < *entry_member) {
        return offset;
      }
    }
    offset = NextMemberAfterScore(*score_offset);
  }
  return std::nullopt;
}

bool ZSetListpack::Delete(std::string_view member) {
  const std::optional<size_t> offset = listpack_.FindAndSkip(member, 1);
  if (!offset.has_value()) {
    return false;
  }
  DeleteMemberScorePair(*offset);
  return true;
}

std::optional<double> ZSetListpack::Score(std::string_view member) const {
  containers::Listpack::IntBuffer member_buffer;
  const std::optional<size_t> offset = listpack_.FindAndSkip(member, 1);
  if (!offset.has_value()) {
    return std::nullopt;
  }
  const std::optional<EntryView> entry = EntryAt(*offset, &member_buffer);
  return entry.has_value() ? std::optional<double>(entry->score) : std::nullopt;
}

std::optional<ZSetRank> ZSetListpack::RankAndScore(
    std::string_view member) const {
  containers::Listpack::IntBuffer member_buffer;
  // Find the member first, comparing bytes without decoding scores, then
  // count the pairs in front of it; its score is read once at the end.
  const std::optional<size_t> member_offset = listpack_.FindAndSkip(member, 1);
  if (!member_offset.has_value()) {
    return std::nullopt;
  }
  size_t rank = 0;
  std::optional<size_t> offset = listpack_.First();
  while (offset.has_value() && offset != member_offset) {
    const std::optional<size_t> score_offset = listpack_.Next(*offset);
    if (!score_offset.has_value()) {
      return std::nullopt;
    }
    offset = NextMemberAfterScore(*score_offset);
    ++rank;
  }
  if (!offset.has_value()) {
    return std::nullopt;
  }
  const std::optional<EntryView> entry =
      EntryAt(*member_offset, &member_buffer);
  if (!entry.has_value()) {
    return std::nullopt;
  }
  return ZSetRank{.rank = rank, .score = entry->score};
}

void ZSetListpack::ForEachInRankRange(const RangeByRankSpec* spec,
                                      const ZSetMemberVisitor& visitor) const {
  if (spec == nullptr) {
    return;
  }
  const std::optional<RangeByRankSpec> rank_spec =
      NormalizeRankRange(*spec, Size());
  if (!rank_spec.has_value() || !ValidateRangeRankSpec(&*rank_spec)) {
    return;
  }
  if (spec->reverse) {
    VisitRankRangeReversed(&*rank_spec, visitor);
  } else {
    VisitRankRange(&*rank_spec, visitor);
  }
}

void ZSetListpack::ForEachInScoreRange(const RangeByScoreSpec* spec,
                                       const ZSetMemberVisitor& visitor) const {
  if (!ValidateRangeScoreSpec(spec)) {
    return;
  }
  if (spec->reverse) {
    VisitScoreRangeReversed(spec, visitor);
  } else {
    VisitScoreRange(spec, visitor);
  }
}

size_t ZSetListpack::Count(const RangeByScoreSpec* spec) const {
  containers::Listpack::IntBuffer member_buffer;
  if (!ValidateRangeScoreSpec(spec)) {
    return 0;
  }
  std::optional<size_t> offset = listpack_.First();
  size_t count = 0;
  while (offset.has_value()) {
    const std::optional<EntryView> entry = EntryAt(*offset, &member_buffer);
    if (!entry.has_value()) {
      break;
    }
    if (IsInRange(entry->score, spec)) {
      ++count;
    } else if (!LessOrEqual(entry->score, spec)) {
      break;
    }
    offset = NextMemberAfterScore(entry->score_offset);
  }
  return count;
}

bool ZSetListpack::OverlapsLexRange(const RangeByLexSpec* spec) const {
  // Redis' zzlIsInLexRange: the last member reaches the minimum and the
  // first member the maximum.
  if (spec == nullptr || spec->Empty()) {
    return false;
  }
  const std::optional<size_t> first = listpack_.First();
  const std::optional<size_t> last_score = listpack_.Last();
  if (!first.has_value() || !last_score.has_value()) {
    return false;
  }
  const std::optional<size_t> last = listpack_.Prev(*last_score);
  containers::Listpack::IntBuffer last_buffer;
  const std::optional<std::string_view> last_member =
      last.has_value() ? ValueAt(*last, &last_buffer) : std::nullopt;
  if (!last_member.has_value() || !spec->AboveMin(*last_member)) {
    return false;
  }
  containers::Listpack::IntBuffer first_buffer;
  const std::optional<std::string_view> first_member =
      ValueAt(*first, &first_buffer);
  return first_member.has_value() && spec->BelowMax(*first_member);
}

void ZSetListpack::ForEachInLexRange(const RangeByLexSpec* spec,
                                     const ZSetMemberVisitor& visitor) const {
  containers::Listpack::IntBuffer member_buffer;
  // Redis' zzlFirstInLexRange and zzlLastInLexRange: walk from one end to
  // the first member inside the range, then on while members stay in it.
  if (!OverlapsLexRange(spec)) {
    return;
  }
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  size_t skip = spec->limit ? spec->limit->offset : 0;
  if (count.has_value() && *count == 0) {
    return;
  }
  size_t visited = 0;
  // Returns whether the walk should go on.
  const auto visit = [&](const EntryView& entry) {
    if (skip > 0) {
      --skip;
      return true;
    }
    visitor(entry.member, entry.score);
    ++visited;
    return !count.has_value() || visited < *count;
  };
  if (!spec->reverse) {
    bool started = false;
    std::optional<size_t> member_offset = listpack_.First();
    while (member_offset.has_value()) {
      const std::optional<EntryView> entry =
          EntryAt(*member_offset, &member_buffer);
      if (!entry.has_value()) {
        break;
      }
      started = started || spec->AboveMin(entry->member);
      if (started && (!spec->BelowMax(entry->member) || !visit(*entry))) {
        break;
      }
      member_offset = NextMemberAfterScore(entry->score_offset);
    }
    return;
  }
  const std::optional<size_t> last_score = listpack_.Last();
  std::optional<size_t> member_offset =
      last_score.has_value() ? listpack_.Prev(*last_score) : std::nullopt;
  bool started = false;
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry =
        EntryAt(*member_offset, &member_buffer);
    if (!entry.has_value()) {
      break;
    }
    started = started || spec->BelowMax(entry->member);
    if (started && (!spec->AboveMin(entry->member) || !visit(*entry))) {
      break;
    }
    member_offset = PrevMemberBeforeMember(entry->member_offset);
  }
}

size_t ZSetListpack::LexCount(const RangeByLexSpec* spec) const {
  containers::Listpack::IntBuffer member_buffer;
  if (!OverlapsLexRange(spec)) {
    return 0;
  }
  size_t count = 0;
  bool started = false;
  std::optional<size_t> member_offset = listpack_.First();
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry =
        EntryAt(*member_offset, &member_buffer);
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
    member_offset = NextMemberAfterScore(entry->score_offset);
  }
  return count;
}

size_t ZSetListpack::Scan(size_t /*cursor*/, size_t /*count*/,
                          const ZSetMemberVisitor& visitor) {
  ForEachEntry([&visitor](std::string_view member, double score) {
    visitor(member, score);
    return true;
  });
  return 0;
}

bool ZSetListpack::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  containers::Listpack::IntBuffer member_buffer;
  std::optional<size_t> member_offset = listpack_.First();
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry =
        EntryAt(*member_offset, &member_buffer);
    if (!entry.has_value()) {
      return false;
    }
    if (!visitor(entry->member, entry->score)) {
      return false;
    }
    member_offset = NextMemberAfterScore(entry->score_offset);
  }
  return true;
}

void ZSetListpack::DeleteMemberScorePair(size_t offset) {
  listpack_.DeleteRange(offset, 2);
}

std::optional<std::string_view> ZSetListpack::ValueAt(
    size_t offset, containers::Listpack::IntBuffer* buffer) const {
  return listpack_.ElementAt(offset, buffer);
}

std::optional<ZSetListpack::EntryView> ZSetListpack::EntryAt(
    size_t member_offset,
    containers::Listpack::IntBuffer* member_buffer) const {
  const std::optional<size_t> score_offset = listpack_.Next(member_offset);
  if (!score_offset.has_value()) {
    return std::nullopt;
  }
  const std::optional<double> score = ScoreAt(*score_offset);
  if (!score.has_value()) {
    return std::nullopt;
  }
  const std::optional<std::string_view> member =
      ValueAt(member_offset, member_buffer);
  if (!member.has_value()) {
    return std::nullopt;
  }
  return EntryView{*member, *score, member_offset, *score_offset};
}

std::optional<double> ZSetListpack::ScoreAt(size_t offset) const {
  // Integral scores are stored with integer encodings. Convert them directly
  // instead of rendering them as text and parsing that text back; the text
  // was formatted from a double, so both paths give the same score.
  if (const std::optional<int64_t> integer = listpack_.IntegerAt(offset);
      integer.has_value()) {
    return static_cast<double>(*integer);
  }
  containers::Listpack::IntBuffer buffer;
  const std::optional<std::string_view> value = ValueAt(offset, &buffer);
  return value.has_value() ? utils::ToDouble(*value) : std::nullopt;
}

void ZSetListpack::VisitRankRange(const RangeByRankSpec* spec,
                                  const ZSetMemberVisitor& visitor) const {
  const std::optional<VisitedRanks> ranks = RanksToVisit(*spec, Size());
  if (!ranks.has_value()) {
    return;
  }
  containers::Listpack::IntBuffer member_buffer;
  // Seeks to the range's first member, as Redis' ZRANGE does with lpSeek,
  // so the members before it are stepped over without reading their scores.
  // A member and its score are two elements.
  std::optional<size_t> offset = listpack_.OffsetAt(2 * ranks->first);
  for (size_t rank = ranks->first; rank < ranks->end && offset.has_value();
       ++rank) {
    const std::optional<EntryView> entry = EntryAt(*offset, &member_buffer);
    if (!entry.has_value()) {
      break;
    }
    visitor(entry->member, entry->score);
    offset = NextMemberAfterScore(entry->score_offset);
  }
}

void ZSetListpack::VisitRankRangeReversed(
    const RangeByRankSpec* spec, const ZSetMemberVisitor& visitor) const {
  const size_t size = Size();
  // Ranks count from the last member here, the one with the greatest score.
  const std::optional<VisitedRanks> ranks = RanksToVisit(*spec, size);
  if (!ranks.has_value()) {
    return;
  }
  containers::Listpack::IntBuffer member_buffer;
  // Seeks as VisitRankRange does. `ranks->first` is less than `size`.
  std::optional<size_t> offset =
      listpack_.OffsetAt(2 * (size - 1 - ranks->first));
  for (size_t rank = ranks->first; rank < ranks->end && offset.has_value();
       ++rank) {
    const std::optional<EntryView> entry = EntryAt(*offset, &member_buffer);
    if (!entry.has_value()) {
      break;
    }
    visitor(entry->member, entry->score);
    offset = PrevMemberBeforeMember(entry->member_offset);
  }
}

void ZSetListpack::VisitScoreRange(const RangeByScoreSpec* spec,
                                   const ZSetMemberVisitor& visitor) const {
  containers::Listpack::IntBuffer member_buffer;
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return;
  }
  std::optional<size_t> member_offset = FindKeyGreaterOrEqual(spec);
  if (!member_offset.has_value()) {
    return;
  }
  size_t visited = 0;
  size_t i = 0;
  size_t offset = spec->limit ? spec->limit->offset : 0;
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry =
        EntryAt(*member_offset, &member_buffer);
    if (!entry.has_value() || !LessOrEqual(entry->score, spec)) {
      break;
    }
    if (IsInRange(entry->score, spec) && i >= offset) {
      visitor(entry->member, entry->score);
      ++visited;
      if (count.has_value() && visited == *count) {
        break;
      }
    }
    member_offset = NextMemberAfterScore(entry->score_offset);
    ++i;
  }
}

void ZSetListpack::VisitScoreRangeReversed(
    const RangeByScoreSpec* spec, const ZSetMemberVisitor& visitor) const {
  containers::Listpack::IntBuffer member_buffer;
  const std::optional<size_t> count =
      spec->limit ? spec->limit->count : std::nullopt;
  if (count.has_value() && *count == 0) {
    return;
  }
  std::optional<size_t> member_offset = FindKeyLessOrEqual(spec);
  if (!member_offset.has_value()) {
    return;
  }
  size_t visited = 0;
  size_t i = 0;
  size_t offset = spec->limit ? spec->limit->offset : 0;
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry =
        EntryAt(*member_offset, &member_buffer);
    if (!entry.has_value() || !GreaterOrEqual(entry->score, spec)) {
      break;
    }
    if (IsInRange(entry->score, spec) && i >= offset) {
      visitor(entry->member, entry->score);
      ++visited;
      if (count.has_value() && visited == *count) {
        break;
      }
    }
    member_offset = PrevMemberBeforeMember(entry->member_offset);
    ++i;
  }
}

std::optional<size_t> ZSetListpack::NextMemberAfterScore(
    size_t score_offset) const {
  return listpack_.Next(score_offset);
}

std::optional<size_t> ZSetListpack::PrevScoreBeforeMember(
    size_t member_offset) const {
  return listpack_.Prev(member_offset);
}

std::optional<size_t> ZSetListpack::PrevMemberBeforeMember(
    size_t member_offset) const {
  const std::optional<size_t> score_offset =
      PrevScoreBeforeMember(member_offset);
  return score_offset.has_value() ? listpack_.Prev(*score_offset)
                                  : std::nullopt;
}

std::optional<size_t> ZSetListpack::FindKeyGreaterOrEqual(
    const RangeByScoreSpec* spec) const {
  containers::Listpack::IntBuffer member_buffer;
  std::optional<size_t> member_offset = listpack_.First();
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry =
        EntryAt(*member_offset, &member_buffer);
    if (!entry.has_value()) {
      break;
    }
    if (GreaterOrEqual(entry->score, spec)) {
      return member_offset;
    }
    member_offset = NextMemberAfterScore(entry->score_offset);
  }
  return std::nullopt;
}

std::optional<size_t> ZSetListpack::FindKeyLessOrEqual(
    const RangeByScoreSpec* spec) const {
  containers::Listpack::IntBuffer member_buffer;
  const std::optional<size_t> score_offset = listpack_.Last();
  if (!score_offset.has_value()) {
    return std::nullopt;
  }
  std::optional<size_t> member_offset = listpack_.Prev(*score_offset);
  while (member_offset.has_value()) {
    const std::optional<EntryView> entry =
        EntryAt(*member_offset, &member_buffer);
    if (!entry.has_value()) {
      break;
    }
    if (LessOrEqual(entry->score, spec)) {
      return member_offset;
    }
    member_offset = PrevMemberBeforeMember(entry->member_offset);
  }
  return std::nullopt;
}

bool ZSetListpack::ValidateRangeRankSpec(const RangeByRankSpec* spec) {
  return (spec != nullptr) && spec->min >= 0 && spec->max >= 0 &&
         ((!spec->minex && !spec->maxex && spec->min <= spec->max) ||
          spec->min < spec->max);
}

bool ZSetListpack::ValidateRangeScoreSpec(const RangeByScoreSpec* spec) {
  return (spec != nullptr) &&
         ((!spec->minex && !spec->maxex && spec->min <= spec->max) ||
          spec->min < spec->max);
}

bool ZSetListpack::IsInRange(double score, const RangeByScoreSpec* spec) {
  return (spec->minex ? score > spec->min : score >= spec->min) &&
         (spec->maxex ? score < spec->max : score <= spec->max);
}

bool ZSetListpack::GreaterOrEqual(double score, const RangeByScoreSpec* spec) {
  return spec->minex ? score > spec->min : score >= spec->min;
}

bool ZSetListpack::LessOrEqual(double score, const RangeByScoreSpec* spec) {
  return spec->maxex ? score < spec->max : score <= spec->max;
}
}  // namespace redis_simple::data_types
