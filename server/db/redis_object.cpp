#include "server/db/redis_object.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

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
  value_.emplace<EmbeddedString>(embedded);
}

StringValue RedisObject::String() const {
  if (const int64_t* integer = std::get_if<int64_t>(&value_)) {
    return StringValue(*integer);
  }
  if (const redis_simple::db::EmbeddedString* embedded =
          std::get_if<EmbeddedString>(&value_)) {
    return StringValue(embedded->View());
  }
  if (const SharedString* shared = std::get_if<SharedString>(&value_)) {
    return StringValue(std::string_view(**shared));
  }
  const auto* value = std::get_if<std::string>(&value_);
  RS_CHECK(value != nullptr);
  return StringValue(std::string_view(*value));
}

std::shared_ptr<const std::string> RedisObject::SharedStringBytes() const {
  const SharedString* shared = std::get_if<SharedString>(&value_);
  return shared == nullptr ? nullptr : *shared;
}

std::string* RedisObject::ExclusiveString() {
  if (auto* const raw = std::get_if<std::string>(&value_)) {
    return raw;
  }
  // Only the main thread takes new references, so a count of one cannot
  // grow while this thread works on the string. A reply that drops its
  // reference on an I/O thread meanwhile only makes the count read late,
  // which costs a copy.
  if (const SharedString* shared = std::get_if<SharedString>(&value_);
      shared != nullptr && shared->use_count() == 1) {
    return shared->get();
  }
  return nullptr;
}

void RedisObject::EmplaceLongString(std::string value) {
  if (value.size() >= kMinSharedStringBytes) {
    value_.emplace<SharedString>(
        std::make_shared<std::string>(std::move(value)));
    return;
  }
  value_.emplace<std::string>(std::move(value));
}

std::optional<int64_t> RedisObject::Int() const {
  if (const int64_t* integer = std::get_if<int64_t>(&value_)) {
    return *integer;
  }
  return utils::ToInt64(String().View());
}

std::string* RedisObject::MutableString() {
  if (const int64_t* integer = std::get_if<int64_t>(&value_)) {
    value_.emplace<std::string>(StringValue(*integer).View());
  } else if (const redis_simple::db::EmbeddedString* embedded =
                 std::get_if<EmbeddedString>(&value_)) {
    // Copy out first: emplacing destroys the embedded bytes.
    std::string raw(embedded->View());
    value_.emplace<std::string>(std::move(raw));
  } else if (const SharedString* shared = std::get_if<SharedString>(&value_)) {
    // The caller edits in place, which a reply holding the bytes must not
    // see: take the buffer when nothing else holds it, and copy otherwise.
    std::string raw = ExclusiveString() != nullptr ? std::move(**shared)
                                                   : std::string(**shared);
    value_.emplace<std::string>(std::move(raw));
  }
  return std::get_if<std::string>(&value_);
}

void RedisObject::SetInt(int64_t value) {
  RS_CHECK(Type() == ObjectType::kString);
  value_.emplace<int64_t>(value);
}

void RedisObject::AssignString(std::string_view value) {
  RS_CHECK(Type() == ObjectType::kString);
  if (const std::optional<int64_t> integer = utils::ToInt64(value);
      integer.has_value()) {
    value_.emplace<int64_t>(*integer);
    return;
  }
  // An embedded value is overwritten in place. memmove also handles a value
  // that views this object's own bytes.
  if (auto* const embedded = std::get_if<EmbeddedString>(&value_);
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
    if (std::holds_alternative<std::string>(value_) &&
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
    value_.emplace<int64_t>(*integer);
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
  if (std::holds_alternative<int64_t>(value_)) {
    return Encoding::kInt;
  }
  if (std::holds_alternative<EmbeddedString>(value_)) {
    return Encoding::kEmbedded;
  }
  return std::holds_alternative<std::string>(value_) ||
                 std::holds_alternative<SharedString>(value_)
             ? Encoding::kRaw
             : Encoding::kOther;
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

bool RedisObject::ReleaseSome(size_t max_elements) {
  if (data_types::Set* const set = Set(); set != nullptr) {
    return set->ReleaseSome(max_elements);
  }
  if (data_types::Hash* const hash = Hash(); hash != nullptr) {
    return hash->ReleaseSome(max_elements);
  }
  if (data_types::ZSet* const zset = ZSet(); zset != nullptr) {
    return zset->ReleaseSome(max_elements);
  }
  // Strings are one block; a list's quicklist nodes hold up to 8 KiB each,
  // so even a long list has few to free.
  return true;
}
}  // namespace redis_simple::db
