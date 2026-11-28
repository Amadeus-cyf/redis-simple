#include "data_types/list/list.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redis_simple::data_types {
List::List(size_t list_max_listpack_bytes)
    : list_max_listpack_bytes_(list_max_listpack_bytes) {}

bool List::LPush(std::string_view value) {
  return Push(value, containers::ListEnd::kHead);
}

bool List::RPush(std::string_view value) {
  return Push(value, containers::ListEnd::kTail);
}

std::optional<std::string> List::RPop() {
  return PopValue(containers::ListEnd::kTail);
}

std::optional<std::string> List::LPop() {
  return PopValue(containers::ListEnd::kHead);
}

std::optional<std::string> List::At(size_t index) const {
  std::optional<std::string> result;
  ForEach(index, index, [&result](std::string_view value) {
    result.emplace(value);
    return true;
  });
  return result;
}

bool List::Set(size_t index, std::string_view value) {
  const size_t size = Size();
  if (index >= size) {
    return false;
  }

  if (listpack_.Allocated()) {
    const std::optional<size_t> offset = listpack_.OffsetAt(index);
    if (!offset.has_value() || !listpack_.Replace(*offset, value)) {
      return false;
    }
    if (listpack_.TotalBytes() > list_max_listpack_bytes_) {
      ConvertListpackToQuicklist();
    }
    return true;
  }

  if (!quicklist_->Set(index, value)) {
    return false;
  }
  // A smaller value can shrink the list enough to use a listpack again.
  TryConvertQuicklistToListpack();
  return true;
}

size_t List::Remove(std::string_view value, size_t limit,
                    containers::ListEnd from) {
  if (listpack_.Allocated()) {
    return listpack_.DeleteMatching(value, limit, from);
  }
  const size_t removed = quicklist_->Remove(value, limit, from);
  TryConvertQuicklistToListpack();
  return removed;
}

void List::Trim(size_t start, size_t stop) {
  const size_t size = Size();
  if (start > stop || start >= size) {
    // Nothing is kept: an empty list is an empty listpack.
    listpack_ = containers::Listpack();
    quicklist_.reset();
    return;
  }
  stop = std::min(stop, size - 1);
  if (start == 0 && stop == size - 1) {
    return;
  }

  if (listpack_.Allocated()) {
    TrimListpack(start, stop);
    return;
  }

  quicklist_->Trim(start, stop);
  TryConvertQuicklistToListpack();
}

void List::TrimListpack(size_t start, size_t stop) {
  const size_t size = ListpackStorage().Size();
  const size_t tail_count = size - stop - 1;
  const std::optional<size_t> tail_offset =
      ListpackStorage().OffsetAt(stop + 1);
  if (tail_offset.has_value()) {
    ListpackStorage().DeleteRange(*tail_offset, tail_count);
  }
  const std::optional<size_t> first = ListpackStorage().First();
  if (first.has_value()) {
    ListpackStorage().DeleteRange(*first, start);
  }
}

size_t List::Size() const {
  return listpack_.Allocated() ? listpack_.Size() : quicklist_->Size();
}

size_t List::NodeCount() const {
  return quicklist_ ? quicklist_->NodeCount() : 0;
}

std::vector<std::string> List::Range(size_t start, size_t stop) const {
  std::vector<std::string> values;
  const size_t size = Size();
  if (start > stop || start >= size) {
    return values;
  }
  stop = std::min(stop, size - 1);
  values.reserve(stop - start + 1);
  ForEach(start, stop, [&values](std::string_view value) {
    values.emplace_back(value);
    return true;
  });
  return values;
}

enum List::Encoding List::Encoding() const {
  return listpack_.Allocated() ? Encoding::kListpack : Encoding::kQuicklist;
}

bool List::Push(std::string_view value, containers::ListEnd end) {
  const bool head = end == containers::ListEnd::kHead;
  if (listpack_.Allocated()) {
    if (!WouldExceedListpackLimit(value)) {
      return head ? listpack_.Prepend(value) : listpack_.Append(value);
    }
    ConvertListpackToQuicklist();
  }
  return head ? quicklist_->LPush(value) : quicklist_->RPush(value);
}

std::optional<std::string> List::PopValue(containers::ListEnd end) {
  std::optional<std::string> value;
  Pop(end, [&value](std::string_view element) { value.emplace(element); });
  return value;
}

bool List::WouldExceedListpackLimit(std::string_view value) const {
  if (!listpack_.Allocated()) {
    return false;
  }
  const size_t bytes = listpack_.TotalBytes();
  const size_t added = containers::Listpack::EstimateEntryBytes(value);
  return bytes > list_max_listpack_bytes_ ||
         added > list_max_listpack_bytes_ - bytes;
}

void List::ConvertListpackToQuicklist() {
  auto quicklist =
      std::make_unique<containers::Quicklist>(list_max_listpack_bytes_);
  quicklist->AdoptListpack(std::move(ListpackStorage()));
  listpack_.Release();
  quicklist_ = std::move(quicklist);
}

void List::TryConvertQuicklistToListpack() {
  if (!quicklist_) {
    return;
  }
  const std::optional<size_t> bytes = quicklist_->ListpackBytes();
  if (!bytes.has_value() || *bytes > list_max_listpack_bytes_ / 2) {
    return;
  }
  listpack_ = quicklist_->ExtractListpack();
  quicklist_.reset();
}
}  // namespace redis_simple::data_types
