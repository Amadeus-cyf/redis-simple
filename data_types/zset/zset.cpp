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
    : encoding_(Encoding::kListPack),
      storage_(std::make_unique<ZSetListPack>()) {}

std::unique_ptr<ZSet> ZSet::CreateForSize(size_t count) {
  std::unique_ptr<ZSet> zset(new ZSet());
  if (count > kListPackMaxEntries) {
    zset->encoding_ = Encoding::kSkiplist;
    zset->storage_ = std::make_unique<ZSetSkiplist>(count);
  }
  return zset;
}

std::unique_ptr<ZSet> ZSet::CreateFromListPack(
    std::unique_ptr<memory::ListPack> listpack) {
  RS_CHECK(listpack != nullptr && listpack->Size() > 0 &&
           listpack->Size() % 2 == 0 &&
           listpack->Size() / 2 <= kListPackMaxEntries);
  std::unique_ptr<ZSet> zset(new ZSet());
  zset->storage_ = std::make_unique<ZSetListPack>(std::move(listpack));
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
  if (encoding_ == Encoding::kListPack &&
      member.size() > kListPackMaxElementLength) {
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
  return encoding_ == Encoding::kListPack &&
         ((inserted && storage_->Size() > kListPackMaxEntries) ||
          member.size() > kListPackMaxElementLength);
}

void ZSet::ConvertAndExpand() {
  RS_CHECK(encoding_ == Encoding::kListPack);
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
