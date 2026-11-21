#include "data_types/set/set.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/check.h"
#include "utils/integers.h"
#include "utils/strings.h"

namespace redis_simple::data_types {
Set::Set()
    : intset_(nullptr),
      listpack_(containers::Listpack::Unallocated()),
      dict_(nullptr) {}

std::unique_ptr<Set> Set::CreateForSize(size_t count) {
  std::unique_ptr<Set> set(new Set());
  if (count > kIntsetMaxEntries) {
    set->ConvertIntsetToDict(count);
  }
  return set;
}

std::unique_ptr<Set> Set::CreateFromListpack(
    std::unique_ptr<containers::Listpack> listpack) {
  RS_CHECK(listpack != nullptr && listpack->Size() > 0 &&
           listpack->Size() <= kListpackMaxEntries);
  std::unique_ptr<Set> set(new Set());
  set->listpack_ = std::move(*listpack);
  return set;
}

bool Set::Add(std::string_view value) {
  if (Encoding() == Encoding::kIntset) {
    return IntsetAddAndMaybeConvert(value);
  }
  if (Encoding() == Encoding::kListpack) {
    return ListpackAddAndMaybeConvert(value);
  }
  if (Encoding() == Encoding::kDict) {
    return DictAdd(value);
  }
  utils::Unreachable();
}

bool Set::HasMember(std::string_view value) const {
  if (Size() == 0) {
    return false;
  }
  if (Encoding() == Encoding::kIntset) {
    const std::optional<int64_t> integer = utils::ToInt64(value);
    return integer.has_value() && intset_->Find(*integer);
  }
  if (Encoding() == Encoding::kListpack) {
    return listpack_.Find(value).has_value();
  }
  if (Encoding() == Encoding::kDict) {
    return dict_->FindValue(value) != nullptr;
  }
  utils::Unreachable();
}

bool Set::HasMember(std::string_view value,
                    const containers::HashedKey* const hashed) const {
  if (hashed != nullptr && Encoding() == Encoding::kDict) {
    return dict_->FindValue(*hashed) != nullptr;
  }
  return HasMember(value);
}

std::vector<std::string> Set::ListAllMembers() const {
  std::vector<std::string> members;
  members.reserve(Size());
  ForEachMember([&members](std::string_view member) {
    members.emplace_back(member);
    return true;
  });
  return members;
}

bool Set::Delete(std::string_view value) {
  if (Size() == 0) {
    return false;
  }
  if (Encoding() == Encoding::kIntset) {
    const std::optional<int64_t> integer = utils::ToInt64(value);
    return integer.has_value() && intset_->Delete(*integer);
  }
  if (Encoding() == Encoding::kListpack) {
    const std::optional<size_t> offset = listpack_.Find(value);
    if (!offset.has_value()) {
      return false;
    }
    listpack_.Delete(*offset);
    return true;
  }
  if (Encoding() == Encoding::kDict) {
    return dict_->Delete(value);
  }
  utils::Unreachable();
}

std::string_view Set::ListpackBytes() const {
  RS_CHECK(Encoding() == Encoding::kListpack);
  return ListpackStorage().Bytes();
}

void Set::AppendIntsetBlob(std::string* const out) const {
  RS_CHECK(Encoding() == Encoding::kIntset);
  intset_->AppendRedisBlob(out);
}

bool Set::ClearSome(size_t max_elements) {
  if (dict_ == nullptr) {
    return true;
  }
  return dict_->ClearSome(max_elements);
}

size_t Set::Size() const {
  switch (Encoding()) {
    case Encoding::kIntset:
      return intset_ ? intset_->Size() : 0;
    case Encoding::kListpack:
      return listpack_.Allocated() ? listpack_.Size() : 0;
    case Encoding::kDict:
      return dict_ ? dict_->Size() : 0;
    default:
      utils::Unreachable();
  }
}

bool Set::IntsetAddAndMaybeConvert(std::string_view value) {
  if (const std::optional<int64_t> integer = utils::ToInt64(value);
      integer.has_value()) {
    if (!intset_) {
      intset_ = std::make_unique<containers::Intset>();
    }
    const bool success = intset_->Add(*integer);
    if (success) {
      MaybeConvertIntsetToDict();
    }
    return success;
  }
  if (!MaybeConvertIntsetToListpack(value)) {
    ConvertIntsetToDict((intset_ ? intset_->Size() : 0) + 1);
    dict_->FindOrInsert(value);
  }
  return true;
}

bool Set::ListpackAddAndMaybeConvert(std::string_view value) {
  const size_t length = value.size();
  if (ListpackStorage().Find(value).has_value()) {
    return false;
  }
  if (ListpackStorage().Size() < kListpackMaxEntries &&
      length <= kListpackMaxElementLength &&
      containers::Listpack::SafeToAdd(
          &ListpackStorage(),
          containers::Listpack::EstimateEntryBytes(value))) {
    return ListpackStorage().Append(value);
  }
  ConvertListpackToDict(ListpackStorage().Size() + 1);
  return dict_->FindOrInsert(value).second;
}

bool Set::DictAdd(std::string_view value) {
  if (!dict_) {
    dict_ = containers::Dict<std::string, containers::NoValue>::Create();
  }
  return dict_->FindOrInsert(value).second;
}

void Set::MaybeConvertIntsetToDict() {
  RS_CHECK(Encoding() == Encoding::kIntset);
  if (intset_ && intset_->Size() > kIntsetMaxEntries) {
    ConvertIntsetToDict(intset_->Size());
  }
}

void Set::ConvertIntsetToDict(size_t capacity) {
  RS_CHECK(Encoding() == Encoding::kIntset);
  dict_ = containers::Dict<std::string, containers::NoValue>::Create(capacity);
  if (!intset_) {
    return;
  }
  for (uint32_t i = 0; i < intset_->Size(); ++i) {
    int64_t value = intset_->Get(i);
    dict_->FindOrInsert(std::to_string(value));
  }
  intset_.reset();
}

bool Set::MaybeConvertIntsetToListpack(std::string_view value) {
  if (Encoding() != Encoding::kIntset) {
    return false;
  }
  size_t length = value.size();
  size_t max_integer_length = 0;
  size_t estimated_bytes = 0;
  const size_t entry_bytes = containers::Listpack::EstimateEntryBytes(value);
  int64_t estimated_integer = 0;
  if (intset_) {
    int64_t max_integer = intset_->Max();
    int64_t min_integer = intset_->Min();
    size_t max_integer_digits = utils::Digits10(max_integer);
    size_t min_integer_digits = utils::Digits10(min_integer);
    max_integer_length = std::max(max_integer_digits, min_integer_digits);
    estimated_integer =
        max_integer_digits > min_integer_digits ? max_integer : min_integer;
    estimated_bytes =
        containers::Listpack::EstimateBytes(estimated_integer, intset_->Size());
  }
  const size_t current_size = intset_ ? intset_->Size() : 0;
  if (current_size < kListpackMaxEntries &&
      length <= kListpackMaxElementLength &&
      max_integer_length <= kListpackMaxElementLength &&
      estimated_bytes <= std::numeric_limits<size_t>::max() - entry_bytes &&
      containers::Listpack::SafeToAdd(nullptr, estimated_bytes + entry_bytes)) {
    ConvertIntsetToListpack(value);
    return true;
  }
  return false;
}

void Set::ConvertIntsetToListpack(std::string_view value) {
  RS_CHECK(Encoding() == Encoding::kIntset);
  listpack_ = containers::Listpack();
  if (intset_) {
    for (uint32_t i = 0; i < intset_->Size(); ++i) {
      listpack_.Append(intset_->Get(i));
    }
  }
  listpack_.Append(value);
  intset_.reset();
}

void Set::ConvertListpackToDict(size_t capacity) {
  RS_CHECK(Encoding() == Encoding::kListpack);
  dict_ = containers::Dict<std::string, containers::NoValue>::Create(capacity);
  if (!listpack_.Allocated()) {
    return;
  }
  if (const size_t size = listpack_.Size(); size > 0) {
    listpack_.ForEach(0, size - 1, [this](std::string_view member) {
      dict_->FindOrInsert(member);
      return true;
    });
  }
  listpack_.Release();
}

}  // namespace redis_simple::data_types
