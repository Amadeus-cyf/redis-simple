#include "server/db/redis_object.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "utils/check.h"
#include "utils/strings.h"

namespace redis_simple::db {
StringValue::StringValue(int64_t value) {
  const int length = utils::Int64ToString(buffer_, value);
  size_ = static_cast<size_t>(length);
}

std::unique_ptr<RedisObject> RedisObject::CreateWithString(std::string value) {
  // ToInt64 rejects forms like "+1" and "007" so GET returns the
  // exact bytes SET stored.
  if (const std::optional<int64_t> integer = utils::ToInt64(value);
      integer.has_value()) {
    return CreateWithInt(*integer);
  }
  if (value.size() <= EmbeddedString::kCapacity) {
    return CreateEmbedded(value);
  }
  if (value.size() >= kMinSharedStringBytes) {
    return Create<SharedString>(
        std::make_shared<std::string>(std::move(value)));
  }
  return Create<std::string>(std::move(value));
}

std::unique_ptr<RedisObject> RedisObject::CreateWithStringCopy(
    std::string_view value) {
  if (const std::optional<int64_t> integer = utils::ToInt64(value);
      integer.has_value()) {
    return CreateWithInt(*integer);
  }
  if (value.size() <= EmbeddedString::kCapacity) {
    return CreateEmbedded(value);
  }
  if (value.size() >= kMinSharedStringBytes) {
    return Create<SharedString>(std::make_shared<std::string>(value));
  }
  return Create<std::string>(value);
}

std::unique_ptr<RedisObject> RedisObject::CreateEmbedded(
    std::string_view value) {
  RS_CHECK(value.size() <= EmbeddedString::kCapacity);
  return Create<EmbeddedString>(value);
}

void RedisObject::EmbedString(std::string_view value) {
  RS_CHECK(value.size() <= EmbeddedString::kCapacity);
  // Copy first: `value` may view the raw buffer that emplacing frees.
  const EmbeddedString embedded(value);
  Emplace<EmbeddedString>(embedded);
}

StringValue RedisObject::String() const {
  if (const auto* integer = GetIf<int64_t>()) {
    return StringValue(*integer);
  }
  if (const auto* embedded = GetIf<EmbeddedString>()) {
    return StringValue(embedded->View());
  }
  if (const auto* shared = GetIf<SharedString>()) {
    return StringValue(std::string_view(**shared));
  }
  const auto* value = GetIf<std::string>();
  RS_CHECK(value != nullptr);
  return StringValue(std::string_view(*value));
}

std::shared_ptr<const std::string> RedisObject::SharedStringBytes() const {
  const auto* shared = GetIf<SharedString>();
  return shared == nullptr ? nullptr : *shared;
}

std::string* RedisObject::ExclusiveString() {
  if (auto* const raw = GetIf<std::string>()) {
    return raw;
  }
  // Only the main thread takes new references, so a count of one cannot
  // grow while this thread works on the string. A reply that drops its
  // reference on an I/O thread meanwhile only makes the count read late,
  // which costs a copy.
  if (const auto* shared = GetIf<SharedString>();
      shared != nullptr && shared->use_count() == 1) {
    return shared->get();
  }
  return nullptr;
}

void RedisObject::EmplaceLongString(std::string value) {
  if (value.size() >= kMinSharedStringBytes) {
    Emplace<SharedString>(std::make_shared<std::string>(std::move(value)));
    return;
  }
  Emplace<std::string>(std::move(value));
}

std::optional<int64_t> RedisObject::Int() const {
  if (const auto* integer = GetIf<int64_t>()) {
    return *integer;
  }
  return utils::ToInt64(String().View());
}

std::string* RedisObject::MutableString() {
  if (const auto* integer = GetIf<int64_t>()) {
    Emplace<std::string>(StringValue(*integer).View());
  } else if (const auto* embedded = GetIf<EmbeddedString>()) {
    // Copy out first: emplacing destroys the embedded bytes.
    std::string raw(embedded->View());
    Emplace<std::string>(std::move(raw));
  } else if (const auto* shared = GetIf<SharedString>()) {
    // The caller edits in place, which a reply holding the bytes must not
    // see: take the buffer when nothing else holds it, and copy otherwise.
    std::string raw = ExclusiveString() != nullptr ? std::move(**shared)
                                                   : std::string(**shared);
    Emplace<std::string>(std::move(raw));
  }
  return GetIf<std::string>();
}

void RedisObject::SetInt(int64_t value) {
  RS_CHECK(Type() == ObjectType::kString);
  Emplace<int64_t>(value);
}

void RedisObject::AssignString(std::string_view value) {
  RS_CHECK(Type() == ObjectType::kString);
  if (const std::optional<int64_t> integer = utils::ToInt64(value);
      integer.has_value()) {
    Emplace<int64_t>(*integer);
    return;
  }
  // An embedded value is overwritten in place. memmove also handles a value
  // that views this object's own bytes.
  if (auto* const embedded = GetIf<EmbeddedString>();
      embedded != nullptr && value.size() <= EmbeddedString::kCapacity) {
    std::memmove(embedded->data.data(), value.data(), value.size());
    embedded->size = static_cast<uint8_t>(value.size());
    return;
  }
  // Short strings live inline, so any buffer up to this size is cheap to keep.
  constexpr size_t kAlwaysReusableBytes = 64;
  std::string* const current = ExclusiveString();
  // An existing buffer that no reply holds is reused when it is small or
  // well filled, which overwrites without allocating. Otherwise a short
  // value is embedded, which also releases an oversized buffer.
  if (current != nullptr && (current->capacity() <= kAlwaysReusableBytes ||
                             current->capacity() / 2 <= value.size())) {
    current->assign(value);
    if (GetIf<std::string>() != nullptr &&
        current->size() >= kMinSharedStringBytes) {
      EmplaceLongString(std::move(*current));
    }
    return;
  }
  if (value.size() <= EmbeddedString::kCapacity) {
    EmbedString(value);
    return;
  }
  // Copy first, in case `value` views this object's current bytes. A new
  // string, rather than an assignment, also frees an oversized buffer.
  EmplaceLongString(std::string(value));
}

void RedisObject::ReplaceWithString(std::string_view value) {
  if (const std::optional<int64_t> integer = utils::ToInt64(value);
      integer.has_value()) {
    Emplace<int64_t>(*integer);
    return;
  }
  if (value.size() <= EmbeddedString::kCapacity) {
    EmbedString(value);
    return;
  }
  // Copy first, in case `value` views this object's current bytes.
  EmplaceLongString(std::string(value));
}

RedisObject::Encoding RedisObject::StringEncoding() const {
  switch (CurrentTag()) {
    case Tag::kInt:
      return Encoding::kInt;
    case Tag::kEmbedded:
      return Encoding::kEmbedded;
    case Tag::kRaw:
    case Tag::kShared:
      return Encoding::kRaw;
    case Tag::kSet:
    case Tag::kList:
    case Tag::kZSet:
    case Tag::kHash:
      return Encoding::kOther;
  }
  utils::Unreachable();
}

std::string_view TypeName(RedisObject::ObjectType type) {
  switch (type) {
    case RedisObject::ObjectType::kString:
      return "string";
    case RedisObject::ObjectType::kSet:
      return "set";
    case RedisObject::ObjectType::kList:
      return "list";
    case RedisObject::ObjectType::kZSet:
      return "zset";
    case RedisObject::ObjectType::kHash:
      return "hash";
  }
  utils::Unreachable();
}

std::optional<RedisObject::ObjectType> TypeFromName(std::string_view name) {
  for (const RedisObject::ObjectType type :
       {RedisObject::ObjectType::kString, RedisObject::ObjectType::kSet,
        RedisObject::ObjectType::kList, RedisObject::ObjectType::kZSet,
        RedisObject::ObjectType::kHash}) {
    if (utils::EqualsIgnoreCase(name, TypeName(type))) {
      return type;
    }
  }
  return std::nullopt;
}

bool RedisObject::ClearSome(size_t max_elements) {
  if (data_types::Set* const set = Set(); set != nullptr) {
    return set->ClearSome(max_elements);
  }
  if (data_types::Hash* const hash = Hash(); hash != nullptr) {
    return hash->ClearSome(max_elements);
  }
  if (data_types::ZSet* const zset = ZSet(); zset != nullptr) {
    return zset->ClearSome(max_elements);
  }
  // Strings are one block; a list's quicklist nodes hold up to 8 KiB each,
  // so even a long list has few to free.
  return true;
}
}  // namespace redis_simple::db
