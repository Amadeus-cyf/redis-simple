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
#include "utils/int_utils.h"
#include "utils/string_utils.h"

namespace redis_simple::set {
Set::Set()
    : encoding_(Encoding::kIntSet),
      intset_(nullptr),
      listpack_(nullptr),
      dict_(nullptr) {}

std::unique_ptr<Set> Set::CreateForSize(size_t count) {
  std::unique_ptr<Set> set(new Set());
  if (count > kIntSetMaxEntries) {
    set->ConvertIntSetToDict(count);
  }
  return set;
}

std::unique_ptr<Set> Set::CreateFromListPack(
    std::unique_ptr<memory::ListPack> listpack) {
  RS_CHECK(listpack != nullptr && listpack->Size() > 0 &&
           listpack->Size() <= kListPackMaxEntries);
  std::unique_ptr<Set> set(new Set());
  set->encoding_ = Encoding::kListPack;
  set->listpack_ = std::move(listpack);
  return set;
}

bool Set::Add(std::string_view value) {
  if (encoding_ == Encoding::kIntSet) {
    return IntSetAddAndMaybeConvert(value);
  }
  if (encoding_ == Encoding::kListPack) {
    return ListPackAddAndMaybeConvert(value);
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
  if (encoding_ == Encoding::kIntSet) {
    int64_t int_value = 0;
    if (!utils::ToInt64(value, &int_value)) {
      return false;
    }
    return intset_->Find(int_value);
  }
  if (encoding_ == Encoding::kListPack) {
    return listpack_->Find(value).has_value();
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->FindValue(value) != nullptr;
  }
  utils::Unreachable();
}

bool Set::HasMember(std::string_view value,
                    const memory::HashedKey* const hashed) const {
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
  if (encoding_ == Encoding::kIntSet) {
    int64_t int_value = 0;
    if (utils::ToInt64(value, &int_value)) {
      return intset_->Delete(int_value);
    }
    return false;
  }
  if (encoding_ == Encoding::kListPack) {
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

std::string_view Set::ListPackBytes() const {
  RS_CHECK(encoding_ == Encoding::kListPack);
  return listpack_->Bytes();
}

void Set::AppendIntSetBlob(std::string* const out) const {
  RS_CHECK(encoding_ == Encoding::kIntSet);
  intset_->AppendRedisBlob(out);
}

size_t Set::Size() const {
  switch (encoding_) {
    case Encoding::kIntSet:
      return intset_ ? intset_->Size() : 0;
    case Encoding::kListPack:
      return listpack_ ? listpack_->Size() : 0;
    case Encoding::kDict:
      return dict_ ? dict_->Size() : 0;
    default:
      utils::Unreachable();
  }
}

bool Set::IntSetAddAndMaybeConvert(std::string_view value) {
  int64_t int_value = 0;
  if (utils::ToInt64(value, &int_value)) {
    if (!intset_) {
      intset_ = std::make_unique<memory::IntSet>();
    }
    bool success = intset_->Add(int_value);
    if (success) {
      MaybeConvertIntsetToDict();
    }
    return success;
  }
  if (!MaybeConvertIntSetToListPack(value)) {
    ConvertIntSetToDict((intset_ ? intset_->Size() : 0) + 1);
    dict_->FindOrInsert(value);
  }
  return true;
}

bool Set::ListPackAddAndMaybeConvert(std::string_view value) {
  const size_t length = value.size();
  if (listpack_->Find(value).has_value()) {
    return false;
  }
  if (listpack_->Size() < kListPackMaxEntries &&
      length <= kListPackMaxElementLength &&
      memory::ListPack::SafeToAdd(
          listpack_.get(), memory::ListPack::EstimateEntryBytes(value))) {
    return listpack_->Append(value);
  }
  ConvertListPackToDict(listpack_->Size() + 1);
  return dict_->FindOrInsert(value).second;
}

bool Set::DictAdd(std::string_view value) {
  if (!dict_) {
    dict_ = memory::Dict<std::string, std::nullptr_t>::Create();
  }
  return dict_->FindOrInsert(value).second;
}

void Set::MaybeConvertIntsetToDict() {
  RS_CHECK(encoding_ == Encoding::kIntSet);
  if (intset_ && intset_->Size() > kIntSetMaxEntries) {
    ConvertIntSetToDict(intset_->Size());
  }
}

void Set::ConvertIntSetToDict(size_t capacity) {
  RS_CHECK(encoding_ == Encoding::kIntSet);
  encoding_ = Encoding::kDict;
  dict_ = memory::Dict<std::string, std::nullptr_t>::Create(capacity);
  if (!intset_) {
    return;
  }
  for (uint32_t i = 0; i < intset_->Size(); ++i) {
    int64_t value = intset_->Get(i);
    dict_->Set(std::to_string(value), nullptr);
  }
  intset_.reset();
}

bool Set::MaybeConvertIntSetToListPack(std::string_view value) {
  if (encoding_ != Encoding::kIntSet) {
    return false;
  }
  size_t length = value.size();
  size_t max_integer_length = 0;
  size_t estimated_bytes = 0;
  const size_t entry_bytes = memory::ListPack::EstimateEntryBytes(value);
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
        memory::ListPack::EstimateBytes(estimated_integer, intset_->Size());
  }
  const size_t current_size = intset_ ? intset_->Size() : 0;
  if (current_size < kListPackMaxEntries &&
      length <= kListPackMaxElementLength &&
      max_integer_length <= kListPackMaxElementLength &&
      estimated_bytes <= std::numeric_limits<size_t>::max() - entry_bytes &&
      memory::ListPack::SafeToAdd(nullptr, estimated_bytes + entry_bytes)) {
    ConvertIntSetToListPack(value);
    return true;
  }
  return false;
}

void Set::ConvertIntSetToListPack(std::string_view value) {
  RS_CHECK(encoding_ == Encoding::kIntSet);
  encoding_ = Encoding::kListPack;
  listpack_ = std::make_unique<memory::ListPack>();
  if (intset_) {
    for (uint32_t i = 0; i < intset_->Size(); ++i) {
      listpack_->Append(intset_->Get(i));
    }
  }
  listpack_->Append(value);
  intset_.reset();
}

void Set::ConvertListPackToDict(size_t capacity) {
  RS_CHECK(encoding_ == Encoding::kListPack);
  encoding_ = Encoding::kDict;
  dict_ = memory::Dict<std::string, std::nullptr_t>::Create(capacity);
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

}  // namespace redis_simple::set
