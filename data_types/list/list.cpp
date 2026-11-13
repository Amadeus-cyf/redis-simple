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
namespace {
containers::Quicklist::RemoveDirection ToQuicklistRemoveDirection(
    List::RemoveDirection direction) {
  return direction == List::RemoveDirection::kFromTail
             ? containers::Quicklist::RemoveDirection::kFromTail
             : containers::Quicklist::RemoveDirection::kFromHead;
}
}  // namespace

List::List(size_t list_max_listpack_bytes)
    : listpack_(std::make_unique<containers::Listpack>()),
      quicklist_(nullptr),
      list_max_listpack_bytes_(list_max_listpack_bytes) {}

bool List::LPush(std::string_view value) { return Push(value, true); }

bool List::RPush(std::string_view value) { return Push(value, false); }

std::optional<std::string> List::RPop() { return Pop(false); }

std::optional<std::string> List::LPop() { return Pop(true); }

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

  if (listpack_) {
    const std::optional<size_t> offset = listpack_->OffsetAt(index);
    if (!offset.has_value() || !listpack_->Replace(*offset, value)) {
      return false;
    }
    if (listpack_->TotalBytes() > list_max_listpack_bytes_) {
      return ConvertListpackToQuicklist();
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

std::optional<size_t> List::Remove(std::string_view value, size_t limit,
                                   RemoveDirection direction) {
  const size_t size = Size();
  if (size == 0) {
    return 0;
  }

  if (listpack_) {
    return RemoveFromListpack(value, limit, direction);
  }

  const std::optional<size_t> removed =
      quicklist_->Remove(value, limit, ToQuicklistRemoveDirection(direction));
  TryConvertQuicklistToListpack();
  return removed;
}

bool List::Trim(size_t start, size_t stop) {
  const size_t size = Size();
  if (size == 0 || start > stop || start >= size) {
    return AdoptReplacement(List::Create(list_max_listpack_bytes_));
  }
  stop = std::min(stop, size - 1);
  if (start == 0 && stop == size - 1) {
    return true;
  }

  if (listpack_) {
    TrimListpack(start, stop);
    return true;
  }

  if (!quicklist_->Trim(start, stop)) {
    return false;
  }
  TryConvertQuicklistToListpack();
  return true;
}

std::optional<size_t> List::RemoveFromListpack(std::string_view value,
                                               size_t limit,
                                               RemoveDirection direction) {
  return listpack_->DeleteMatching(value, limit,
                                   direction == RemoveDirection::kFromTail);
}

void List::TrimListpack(size_t start, size_t stop) {
  const size_t size = listpack_->Size();
  const size_t tail_count = size - stop - 1;
  const std::optional<size_t> tail_offset = listpack_->OffsetAt(stop + 1);
  if (tail_offset.has_value()) {
    listpack_->DeleteRange(*tail_offset, tail_count);
  }
  const std::optional<size_t> first = listpack_->First();
  if (first.has_value()) {
    listpack_->DeleteRange(*first, start);
  }
}

size_t List::Size() const {
  return listpack_ ? listpack_->Size() : quicklist_->Size();
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
  return listpack_ ? Encoding::kListpack : Encoding::kQuicklist;
}

bool List::Push(std::string_view value, bool head) {
  if (listpack_) {
    if (WouldExceedListpackLimit(value) && !ConvertListpackToQuicklist()) {
      return false;
    }
    if (listpack_) {
      return head ? listpack_->Prepend(value) : listpack_->Append(value);
    }
  }
  return head ? quicklist_->LPush(value) : quicklist_->RPush(value);
}

std::optional<std::string> List::Pop(bool head) {
  if (listpack_) {
    if (listpack_->Size() == 0) {
      return std::nullopt;
    }
    const std::optional<size_t> offset =
        head ? listpack_->First() : listpack_->Last();
    if (!offset.has_value()) {
      return std::nullopt;
    }
    std::optional<std::string> value = listpack_->Get(*offset);
    listpack_->Delete(*offset);
    return value;
  }

  std::optional<std::string> value =
      head ? quicklist_->LPop() : quicklist_->RPop();
  TryConvertQuicklistToListpack();
  return value;
}

bool List::AdoptReplacement(std::unique_ptr<List> replacement) {
  listpack_ = std::move(replacement->listpack_);
  quicklist_ = std::move(replacement->quicklist_);
  return true;
}

bool List::WouldExceedListpackLimit(std::string_view value) const {
  if (!listpack_) {
    return false;
  }
  const size_t bytes = listpack_->TotalBytes();
  const size_t added = containers::Listpack::EstimateEntryBytes(value);
  return bytes > list_max_listpack_bytes_ ||
         added > list_max_listpack_bytes_ - bytes;
}

bool List::ConvertListpackToQuicklist() {
  auto quicklist =
      std::make_unique<containers::Quicklist>(list_max_listpack_bytes_);
  // The elements stay in the quicklist even if splitting an oversized node
  // fails, so the list is never left without its contents.
  const bool adopted = quicklist->AdoptListpack(std::move(listpack_));
  quicklist_ = std::move(quicklist);
  return adopted;
}

void List::TryConvertQuicklistToListpack() {
  if (!quicklist_) {
    return;
  }
  const std::optional<size_t> bytes = quicklist_->ListpackBytes();
  if (!bytes.has_value() || *bytes > list_max_listpack_bytes_ / 2) {
    return;
  }
  listpack_ = quicklist_->ReleaseListpack();
  quicklist_.reset();
}
}  // namespace redis_simple::data_types
