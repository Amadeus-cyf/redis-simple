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
    : encoding_(Encoding::kIntset),
      intset_(nullptr),
      listpack_(nullptr),
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
  set->encoding_ = Encoding::kListpack;
  set->listpack_ = std::move(listpack);
  return set;
}

bool Set::Add(std::string_view value) {
  if (encoding_ == Encoding::kIntset) {
    return IntsetAddAndMaybeConvert(value);
  }
  if (encoding_ == Encoding::kListpack) {
    return ListpackAddAndMaybeConvert(value);
  }
  if (encoding_ == Encoding::kDict) {
    return DictAdd(value);
  }
  utils::Unreachable();
}

bool Set::HasMember(std::string_view value) const {
  if (Size() == 0) {
    return false;
  }
  if (encoding_ == Encoding::kIntset) {
    int64_t int_value = 0;
    if (!utils::ToInt64(value, &int_value)) {
      return false;
    }
    return intset_->Find(int_value);
  }
  if (encoding_ == Encoding::kListpack) {
    return listpack_->Find(value).has_value();
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->FindValue(value) != nullptr;
  }
  utils::Unreachable();
}

bool Set::HasMember(std::string_view value,
                    const containers::HashedKey* const hashed) const {
  if (hashed != nullptr && encoding_ == Encoding::kDict) {
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
  if (encoding_ == Encoding::kIntset) {
    int64_t int_value = 0;
    if (utils::ToInt64(value, &int_value)) {
      return intset_->Delete(int_value);
    }
    return false;
  }
  if (encoding_ == Encoding::kListpack) {
    const std::optional<size_t> offset = listpack_->Find(value);
    if (!offset.has_value()) {
      return false;
    }
    listpack_->Delete(*offset);
    return true;
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->Delete(value);
  }
  utils::Unreachable();
}

std::string_view Set::ListpackBytes() const {
  RS_CHECK(encoding_ == Encoding::kListpack);
  return listpack_->Bytes();
}

void Set::AppendIntsetBlob(std::string* const out) const {
  RS_CHECK(encoding_ == Encoding::kIntset);
  intset_->AppendRedisBlob(out);
}

size_t Set::Size() const {
  switch (encoding_) {
    case Encoding::kIntset:
      return intset_ ? intset_->Size() : 0;
    case Encoding::kListpack:
      return listpack_ ? listpack_->Size() : 0;
    case Encoding::kDict:
      return dict_ ? dict_->Size() : 0;
    default:
      utils::Unreachable();
  }
}

bool Set::IntsetAddAndMaybeConvert(std::string_view value) {
  int64_t int_value = 0;
  if (utils::ToInt64(value, &int_value)) {
    if (!intset_) {
      intset_ = std::make_unique<containers::Intset>();
    }
    bool success = intset_->Add(int_value);
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
  if (listpack_->Find(value).has_value()) {
    return false;
  }
  if (listpack_->Size() < kListpackMaxEntries &&
      length <= kListpackMaxElementLength &&
      containers::Listpack::SafeToAdd(
          listpack_.get(), containers::Listpack::EstimateEntryBytes(value))) {
    return listpack_->Append(value);
  }
  ConvertListpackToDict(listpack_->Size() + 1);
  return dict_->FindOrInsert(value).second;
}

bool Set::DictAdd(std::string_view value) {
  if (!dict_) {
    dict_ = containers::Dict<std::string, std::nullptr_t>::Create();
  }
  return dict_->FindOrInsert(value).second;
}

void Set::MaybeConvertIntsetToDict() {
  RS_CHECK(encoding_ == Encoding::kIntset);
  if (intset_ && intset_->Size() > kIntsetMaxEntries) {
    ConvertIntsetToDict(intset_->Size());
  }
}

void Set::ConvertIntsetToDict(size_t capacity) {
  RS_CHECK(encoding_ == Encoding::kIntset);
  encoding_ = Encoding::kDict;
  dict_ = containers::Dict<std::string, std::nullptr_t>::Create(capacity);
  if (!intset_) {
    return;
  }
  for (uint32_t i = 0; i < intset_->Size(); ++i) {
    int64_t value = intset_->Get(i);
    dict_->Set(std::to_string(value), nullptr);
  }
  intset_.reset();
}

bool Set::MaybeConvertIntsetToListpack(std::string_view value) {
  if (encoding_ != Encoding::kIntset) {
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
  RS_CHECK(encoding_ == Encoding::kIntset);
  encoding_ = Encoding::kListpack;
  listpack_ = std::make_unique<containers::Listpack>();
  if (intset_) {
    for (uint32_t i = 0; i < intset_->Size(); ++i) {
      listpack_->Append(intset_->Get(i));
    }
  }
  listpack_->Append(value);
  intset_.reset();
}

void Set::ConvertListpackToDict(size_t capacity) {
  RS_CHECK(encoding_ == Encoding::kListpack);
  encoding_ = Encoding::kDict;
  dict_ = containers::Dict<std::string, std::nullptr_t>::Create(capacity);
  if (!listpack_) {
    return;
  }
  std::optional<size_t> offset = listpack_->First();
  while (offset.has_value()) {
    std::optional<std::string> string_result = listpack_->Get(*offset);
    if (string_result.has_value()) {
      dict_->Set(std::move(*string_result), nullptr);
    }
    offset = listpack_->Next(*offset);
  }
  listpack_.reset();
}

}  // namespace redis_simple::data_types
