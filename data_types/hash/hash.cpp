#include "data_types/hash/hash.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/check.h"

namespace redis_simple::data_types {
namespace {
bool CanStoreInListpack(std::string_view field, std::string_view value) {
  return field.size() <= Hash::kListpackMaxElementLength &&
         value.size() <= Hash::kListpackMaxElementLength;
}

}  // namespace

Hash::Hash()
    : encoding_(Encoding::kListpack),
      listpack_(std::make_unique<containers::Listpack>()),
      dict_(nullptr) {}

std::unique_ptr<Hash> Hash::CreateForSize(size_t count) {
  std::unique_ptr<Hash> hash(new Hash());
  if (count > kListpackMaxEntries) {
    hash->ConvertListpackToDict(count);
  }
  return hash;
}

std::unique_ptr<Hash> Hash::CreateFromListpack(
    std::unique_ptr<containers::Listpack> listpack) {
  RS_CHECK(listpack != nullptr && listpack->Size() > 0 &&
           listpack->Size() % 2 == 0 &&
           listpack->Size() / 2 <= kListpackMaxEntries);
  std::unique_ptr<Hash> hash(new Hash());
  hash->listpack_ = std::move(listpack);
  return hash;
}

bool Hash::Set(std::string_view field, std::string_view value) {
  if (encoding_ == Encoding::kListpack) {
    return SetListpack(field, value);
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
  if (encoding_ == Encoding::kListpack) {
    const std::optional<size_t> field_offset = FindListpackField(field);
    if (!field_offset.has_value()) {
      return false;
    }
    const auto offset = static_cast<size_t>(*field_offset);
    listpack_->DeleteRange(offset, 2);
    return true;
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->Delete(field);
  }
  utils::Unreachable();
}

bool Hash::Exists(std::string_view field) const {
  if (encoding_ == Encoding::kListpack) {
    return FindListpackField(field).has_value();
  }
  if (encoding_ == Encoding::kDict) {
    return dict_->FindValue(std::string_view(field)) != nullptr;
  }
  utils::Unreachable();
}

std::string_view Hash::ListpackBytes() const {
  RS_CHECK(encoding_ == Encoding::kListpack);
  return listpack_->Bytes();
}

size_t Hash::Size() const {
  if (encoding_ == Encoding::kListpack) {
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

bool Hash::CanAppendToListpack(std::string_view field,
                               std::string_view value) const {
  return Size() < kListpackMaxEntries && CanStoreInListpack(field, value) &&
         containers::Listpack::SafeToAdd(
             listpack_.get(),
             containers::Listpack::EstimateEntryBytes(field) +
                 containers::Listpack::EstimateEntryBytes(value));
}

bool Hash::SetListpack(std::string_view field, std::string_view value) {
  RS_CHECK(encoding_ == Encoding::kListpack);
  const std::optional<size_t> field_offset = FindListpackField(field);
  if (field_offset.has_value()) {
    if (!CanStoreInListpack(field, value)) {
      ConvertListpackToDict(Size());
      SetDict(field, value);
      return false;
    }
    const std::optional<size_t> value_offset = listpack_->Next(*field_offset);
    if (!value_offset.has_value()) {
      return false;
    }
    listpack_->Replace(*value_offset, value);
    return false;
  }

  if (!CanAppendToListpack(field, value)) {
    ConvertListpackToDict(Size() + 1);
    SetDict(field, value);
    return true;
  }
  return listpack_->BatchAppend({{field, 0, false}, {value, 0, false}});
}

bool Hash::SetDict(std::string_view field, std::string_view value) {
  RS_CHECK(encoding_ == Encoding::kDict);
  if (dict_ == nullptr) {
    dict_ = containers::Dict<std::string, std::string>::Create();
  }
  auto [stored, inserted] = dict_->FindOrInsert(field);
  stored->assign(value);
  return inserted;
}

void Hash::ConvertListpackToDict(size_t capacity) {
  RS_CHECK(encoding_ == Encoding::kListpack);
  dict_ = containers::Dict<std::string, std::string>::Create(capacity);
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

std::optional<size_t> Hash::FindListpackField(std::string_view field) const {
  RS_CHECK(encoding_ == Encoding::kListpack);
  return listpack_->FindAndSkip(field, 1);
}
}  // namespace redis_simple::data_types
