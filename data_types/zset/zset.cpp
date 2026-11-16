#include "data_types/zset/zset.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "data_types/zset/zset_listpack.h"
#include "data_types/zset/zset_skiplist.h"
#include "utils/check.h"

namespace redis_simple::data_types {
ZSet::ZSet() : skiplist_(nullptr) {}

std::unique_ptr<ZSet> ZSet::CreateForSize(size_t count) {
  std::unique_ptr<ZSet> zset(new ZSet());
  if (count > kListpackMaxEntries) {
    zset->listpack_.Release();
    zset->skiplist_ = std::make_unique<ZSetSkiplist>(count);
  }
  return zset;
}

std::unique_ptr<ZSet> ZSet::CreateFromListpack(
    std::unique_ptr<containers::Listpack> listpack) {
  RS_CHECK(listpack != nullptr && listpack->Size() > 0 &&
           listpack->Size() % 2 == 0 &&
           listpack->Size() / 2 <= kListpackMaxEntries);
  std::unique_ptr<ZSet> zset(new ZSet());
  zset->listpack_ = ZSetListpack(std::move(listpack));
  return zset;
}

ZSetAddResult ZSet::Add(std::string_view member, double score,
                        const ZSetAddRules& rules) {
  if (std::isnan(score)) {
    return {.status = ZSetAddStatus::kNotANumber, .score = score};
  }
  if (Encoding() == Encoding::kListpack &&
      member.size() > kListpackMaxElementLength) {
    // A listpack holds no member this long, so XX has nothing to update.
    if (rules.xx) {
      return {.status = ZSetAddStatus::kSkipped, .score = score};
    }
    ConvertAndExpand();
  }
  const ZSetAddResult result = Storage().Add(member, score, rules);
  if (ShouldConvertToSkiplist(member, result.status == ZSetAddStatus::kAdded)) {
    ConvertAndExpand();
  }
  return result;
}

bool ZSet::InsertOrUpdate(std::string_view member, double score,
                          bool* const changed) {
  const ZSetAddStatus status = Add(member, score, ZSetAddRules{}).status;
  if (changed != nullptr) {
    *changed =
        status == ZSetAddStatus::kAdded || status == ZSetAddStatus::kUpdated;
  }
  return status == ZSetAddStatus::kAdded;
}

bool ZSet::Delete(std::string_view member) { return Storage().Delete(member); }

std::optional<double> ZSet::Score(std::string_view member) const {
  return Storage().Score(member);
}

size_t ZSet::Count(const RangeByScoreSpec* spec) const {
  return Storage().Count(spec);
}

bool ZSet::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  return Storage().ForEachEntry(visitor);
}

bool ZSet::ForEachSkiplistEntryDescending(
    const ZSetEntryVisitor& visitor) const {
  RS_CHECK(Encoding() == Encoding::kSkiplist);
  return Skiplist()->ForEachEntryDescending(visitor);
}

enum ZSet::Encoding ZSet::Encoding() const {
  return listpack_.Allocated() ? Encoding::kListpack : Encoding::kSkiplist;
}

bool ZSet::ShouldConvertToSkiplist(std::string_view member,
                                   bool inserted) const {
  return Encoding() == Encoding::kListpack &&
         ((inserted && Storage().Size() > kListpackMaxEntries) ||
          member.size() > kListpackMaxElementLength);
}

void ZSet::ConvertAndExpand() {
  RS_CHECK(Encoding() == Encoding::kListpack);
  auto zset_skiplist = std::make_unique<ZSetSkiplist>();
  if (!Storage().ForEachEntry(
          [&zset_skiplist](std::string_view member, double score) {
            return zset_skiplist->InsertOrUpdate(member, score, nullptr);
          })) {
    return;
  }
  skiplist_ = std::move(zset_skiplist);
  listpack_.Release();
}
}  // namespace redis_simple::data_types
