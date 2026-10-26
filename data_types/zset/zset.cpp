#include "zset.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "data_types/zset/zset_listpack.h"
#include "data_types/zset/zset_skiplist.h"

namespace redis_simple::zset {
ZSet::ZSet()
    : encoding_(Encoding::kListPack),
      storage_(std::make_unique<ZSetListPack>()) {}

/*
 * Insert a new element with score or update the score of an existing
 * element. Return true if the element is newly inserted.
 */
bool ZSet::InsertOrUpdate(std::string_view key, double score) {
  if (std::isnan(score)) {
    return false;
  }
  if (encoding_ == Encoding::kListPack &&
      key.size() > kListPackMaxElementLength) {
    ConvertAndExpand();
  }
  bool inserted = storage_->InsertOrUpdate(key, score);
  if (ShouldConvertToSkiplist(key, inserted)) {
    ConvertAndExpand();
  }
  return inserted;
}

bool ZSet::Delete(std::string_view key) { return storage_->Delete(key); }

std::optional<double> ZSet::Score(std::string_view key) const {
  return storage_->Score(key);
}

std::optional<size_t> ZSet::Rank(std::string_view key) const {
  return storage_->Rank(key);
}

ZSetEntryList ZSet::RangeByRank(const RangeByRankSpec* spec) const {
  return storage_->RangeByRank(spec);
}

ZSetEntryList ZSet::RangeByScore(const RangeByScoreSpec* spec) const {
  return storage_->RangeByScore(spec);
}

/*
 * Count number of elements within the range of the score.
 */
size_t ZSet::Count(const RangeByScoreSpec* spec) const {
  return storage_->Count(spec);
}

bool ZSet::ForEachEntry(const ZSetEntryVisitor& visitor) const {
  return storage_->ForEachEntry(visitor);
}

enum ZSet::Encoding ZSet::Encoding() const { return encoding_; }

bool ZSet::ShouldConvertToSkiplist(std::string_view key, bool inserted) const {
  return encoding_ == Encoding::kListPack &&
         ((inserted && storage_->Size() > kListPackMaxEntries) ||
          key.size() > kListPackMaxElementLength);
}

void ZSet::ConvertAndExpand() {
  assert(encoding_ == Encoding::kListPack);
  auto zset_skiplist = std::make_unique<ZSetSkiplist>();
  if (!storage_->ForEachEntry(
          [&zset_skiplist](std::string_view key, double score) {
            return zset_skiplist->InsertOrUpdate(key, score);
          })) {
    return;
  }
  encoding_ = Encoding::kSkiplist;
  storage_ = std::move(zset_skiplist);
}
}  // namespace redis_simple::zset
