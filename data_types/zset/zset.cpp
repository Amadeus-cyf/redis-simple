#include "data_types/zset/zset.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "data_types/zset/zset_listpack.h"
#include "data_types/zset/zset_skiplist.h"
#include "utils/check.h"

namespace redis_simple::zset {
ZSet::ZSet()
    : encoding_(Encoding::kListpack),
      storage_(std::make_unique<ZSetListpack>()) {}

std::unique_ptr<ZSet> ZSet::CreateForSize(size_t count) {
  std::unique_ptr<ZSet> zset(new ZSet());
  if (count > kListpackMaxEntries) {
    zset->encoding_ = Encoding::kSkiplist;
    zset->storage_ = std::make_unique<ZSetSkiplist>(count);
  }
  return zset;
}

std::unique_ptr<ZSet> ZSet::CreateFromListpack(
    std::unique_ptr<memory::Listpack> listpack) {
  RS_CHECK(listpack != nullptr && listpack->Size() > 0 &&
           listpack->Size() % 2 == 0 &&
           listpack->Size() / 2 <= kListpackMaxEntries);
  std::unique_ptr<ZSet> zset(new ZSet());
  zset->storage_ = std::make_unique<ZSetListpack>(std::move(listpack));
  return zset;
}

bool ZSet::InsertOrUpdate(std::string_view member, double score,
                          bool* const changed) {
  if (changed != nullptr) {
    *changed = false;
  }
  if (std::isnan(score)) {
    return false;
  }
  if (encoding_ == Encoding::kListpack &&
      member.size() > kListpackMaxElementLength) {
    ConvertAndExpand();
  }
  const bool inserted = storage_->InsertOrUpdate(member, score, changed);
  if (ShouldConvertToSkiplist(member, inserted)) {
    ConvertAndExpand();
  }
  return inserted;
}

bool ZSet::Delete(std::string_view member) { return storage_->Delete(member); }

std::optional<double> ZSet::Score(std::string_view member) const {
  return storage_->Score(member);
}

std::optional<size_t> ZSet::Rank(std::string_view member) const {
  return storage_->Rank(member);
}

size_t ZSet::Count(const RangeByScoreSpec* spec) const {
  return storage_->Count(spec);
}

bool ZSet::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  return storage_->ForEachEntry(visitor);
}

enum ZSet::Encoding ZSet::Encoding() const { return encoding_; }

bool ZSet::ShouldConvertToSkiplist(std::string_view member,
                                   bool inserted) const {
  return encoding_ == Encoding::kListpack &&
         ((inserted && storage_->Size() > kListpackMaxEntries) ||
          member.size() > kListpackMaxElementLength);
}

void ZSet::ConvertAndExpand() {
  RS_CHECK(encoding_ == Encoding::kListpack);
  auto zset_skiplist = std::make_unique<ZSetSkiplist>();
  if (!storage_->ForEachEntry(
          [&zset_skiplist](std::string_view member, double score) {
            return zset_skiplist->InsertOrUpdate(member, score, nullptr);
          })) {
    return;
  }
  encoding_ = Encoding::kSkiplist;
  storage_ = std::move(zset_skiplist);
}
}  // namespace redis_simple::zset
