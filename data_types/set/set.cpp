#include "set.h"

#include <algorithm>
#include <cassert>
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
  RS_UNREACHABLE();
}

bool Set::HasMember(std::string_view value) const {
  if (Size() == 0) {
    return false;
  }
  if (encoding_ == Encoding::kIntSet) {
    int64_t int_val = 0;
    if (!utils::ToCanonicalInt64(value, &int_val)) {
      return false;
    }
    return intset_->Find(int_val);
  }
  if (encoding_ == Encoding::kListPack) {
    return listpack_->Find(value).has_value();
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->FindValue(value) != nullptr;
  }
  RS_UNREACHABLE();
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

bool Set::Remove(std::string_view value) {
  if (Size() == 0) {
    return false;
  }
  if (encoding_ == Encoding::kIntSet) {
    int64_t int_val = 0;
    if (utils::ToCanonicalInt64(value, &int_val)) {
      return intset_->Remove(int_val);
    }
    return false;
  }
  if (encoding_ == Encoding::kListPack) {
    const auto idx = listpack_->Find(value);
    if (!idx.has_value()) {
      return false;
    }
    listpack_->Delete(*idx);
    return true;
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->Delete(value);
  }
  RS_UNREACHABLE();
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
      RS_UNREACHABLE();
  }
}

bool Set::IntSetAddAndMaybeConvert(std::string_view value) {
  int64_t int_val = 0;
  if (utils::ToCanonicalInt64(value, &int_val)) {
    if (!intset_) {
      intset_ = std::make_unique<in_memory::IntSet>();
    }
    bool success = intset_->Add(int_val);
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
  const size_t len = value.size();
  if (listpack_->Find(value).has_value()) {
    return false;
  }
  if (listpack_->Size() < kListPackMaxEntries &&
      len <= kListPackElementMaxLength &&
      in_memory::ListPack::SafeToAdd(
          listpack_.get(), in_memory::ListPack::EstimateEntryBytes(value))) {
    return listpack_->Append(value);
  }
  ConvertListPackToDict(listpack_->Size() + 1);
  return dict_->FindOrInsert(value).second;
}

bool Set::DictAdd(std::string_view value) {
  if (!dict_) {
    dict_ = in_memory::Dict<std::string, std::nullptr_t>::Create();
  }
  return dict_->FindOrInsert(value).second;
}

void Set::MaybeConvertIntsetToDict() {
  assert(encoding_ == Encoding::kIntSet);
  if (intset_ && intset_->Size() > kIntSetMaxEntries) {
    ConvertIntSetToDict(intset_->Size());
  }
}

void Set::ConvertIntSetToDict(size_t capacity) {
  assert(encoding_ == Encoding::kIntSet);
  encoding_ = Encoding::kDict;
  dict_ = in_memory::Dict<std::string, std::nullptr_t>::Create(capacity);
  if (!intset_) {
    return;
  }
  for (unsigned int i = 0; i < intset_->Size(); ++i) {
    int64_t value = intset_->Get(i);
    dict_->Set(std::to_string(value), nullptr);
  }
  intset_.reset();
}

bool Set::MaybeConvertIntSetToListPack(std::string_view val) {
  if (encoding_ != Encoding::kIntSet) {
    return false;
  }
  size_t len = val.size();
  size_t max_integer_length = 0;
  size_t estimated_bytes = 0;
  const size_t entry_bytes = in_memory::ListPack::EstimateEntryBytes(val);
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
        in_memory::ListPack::EstimateBytes(estimated_integer, intset_->Size());
  }
  const size_t current_size = intset_ ? intset_->Size() : 0;
  if (current_size < kListPackMaxEntries && len <= kListPackElementMaxLength &&
      max_integer_length <= kListPackElementMaxLength &&
      estimated_bytes <= std::numeric_limits<size_t>::max() - entry_bytes &&
      in_memory::ListPack::SafeToAdd(nullptr, estimated_bytes + entry_bytes)) {
    ConvertIntSetToListPack(val);
    return true;
  }
  return false;
}

void Set::ConvertIntSetToListPack(std::string_view val) {
  assert(encoding_ == Encoding::kIntSet);
  encoding_ = Encoding::kListPack;
  listpack_ = std::make_unique<in_memory::ListPack>();
  if (intset_) {
    for (unsigned int i = 0; i < intset_->Size(); ++i) {
      int64_t value = intset_->Get(i);
      listpack_->Append(value);
    }
  }
  listpack_->Append(val);
  intset_.reset();
}

void Set::ConvertListPackToDict(size_t capacity) {
  assert(encoding_ == Encoding::kListPack);
  encoding_ = Encoding::kDict;
  dict_ = in_memory::Dict<std::string, std::nullptr_t>::Create(capacity);
  if (!listpack_) {
    return;
  }
  auto idx = listpack_->First();
  while (idx.has_value()) {
    auto string_result = listpack_->Get(*idx);
    if (string_result.has_value()) {
      dict_->Set(std::move(*string_result), nullptr);
    }
    idx = listpack_->Next(*idx);
  }
  listpack_.reset();
}

}  // namespace redis_simple::set
