#include "data_types/hash/hash.h"

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/check.h"

namespace redis_simple::hash {
namespace {
constexpr size_t kListPackMaxEntries = 128;
constexpr size_t kListPackMaxElementLength = 64;

bool CanStoreInListPack(std::string_view field, std::string_view value) {
  return field.size() <= kListPackMaxElementLength &&
         value.size() <= kListPackMaxElementLength;
}

}  // namespace

Hash::Hash()
    : encoding_(Encoding::kListPack),
      listpack_(std::make_unique<in_memory::ListPack>()),
      dict_(nullptr) {}

bool Hash::Set(std::string_view field, std::string_view value) {
  if (encoding_ == Encoding::kListPack) {
    return SetListPack(field, value);
  }
  if (encoding_ == Encoding::kDict) {
    return SetDict(field, value);
  }
  utils::Unreachable();
}

std::optional<std::string> Hash::Get(std::string_view field) const {
  std::optional<std::string> result;
  VisitValue(field,
             [&result](std::string_view value) { result.emplace(value); });
  return result;
}

bool Hash::Delete(std::string_view field) {
  if (encoding_ == Encoding::kListPack) {
    const auto field_idx = FindListPackField(field);
    if (!field_idx.has_value()) {
      return false;
    }
    const auto idx = static_cast<size_t>(*field_idx);
    listpack_->DeleteRange(idx, 2);
    return true;
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->Delete(field);
  }
  utils::Unreachable();
}

bool Hash::Exists(std::string_view field) const {
  if (encoding_ == Encoding::kListPack) {
    return FindListPackField(field).has_value();
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->FindValue(std::string_view(field)) != nullptr;
  }
  utils::Unreachable();
}

size_t Hash::Size() const {
  if (encoding_ == Encoding::kListPack) {
    return listpack_ == nullptr ? 0 : listpack_->Size() / 2;
  }
  if (encoding_ == Encoding::kDict) {
    return dict_ == nullptr ? 0 : dict_->Size();
  }
  utils::Unreachable();
}

std::vector<Hash::Entry> Hash::Entries() const {
  std::vector<Entry> entries;
  entries.reserve(Size());
  ForEachEntry([&entries](std::string_view field, std::string_view value) {
    entries.push_back({std::string(field), std::string(value)});
    return true;
  });
  return entries;
}

bool Hash::CanAppendToListPack(std::string_view field,
                               std::string_view value) const {
  return Size() < kListPackMaxEntries && CanStoreInListPack(field, value) &&
         in_memory::ListPack::SafeToAdd(
             listpack_.get(),
             in_memory::ListPack::EstimateEntryBytes(field) +
                 in_memory::ListPack::EstimateEntryBytes(value));
}

bool Hash::SetListPack(std::string_view field, std::string_view value) {
  assert(encoding_ == Encoding::kListPack);
  const auto field_idx = FindListPackField(field);
  if (field_idx.has_value()) {
    if (!CanStoreInListPack(field, value)) {
      ConvertListPackToDict(Size());
      SetDict(field, value);
      return false;
    }
    const auto value_idx = listpack_->Next(*field_idx);
    if (!value_idx.has_value()) {
      return false;
    }
    listpack_->Replace(*value_idx, value);
    return false;
  }

  if (!CanAppendToListPack(field, value)) {
    ConvertListPackToDict(Size() + 1);
    SetDict(field, value);
    return true;
  }
  return listpack_->BatchAppend({{field, 0, false}, {value, 0, false}});
}

bool Hash::SetDict(std::string_view field, std::string_view value) {
  assert(encoding_ == Encoding::kDict);
  if (dict_ == nullptr) {
    dict_ = in_memory::Dict<std::string, std::string>::Create();
  }
  auto [stored, inserted] = dict_->FindOrInsert(field);
  stored->assign(value);
  return inserted;
}

void Hash::ConvertListPackToDict(size_t capacity) {
  assert(encoding_ == Encoding::kListPack);
  dict_ = in_memory::Dict<std::string, std::string>::Create(capacity);
  if (listpack_ != nullptr) {
    listpack_->ForEachPair(
        [this](std::string_view field, std::string_view value) {
          dict_->Set(std::string(field), std::string(value));
          return true;
        });
  }
  listpack_.reset();
  encoding_ = Encoding::kDict;
}

std::optional<size_t> Hash::FindListPackField(std::string_view field) const {
  assert(encoding_ == Encoding::kListPack);
  return listpack_->FindAndSkip(field, 1);
}
}  // namespace redis_simple::hash
