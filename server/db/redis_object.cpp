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
  int64_t integer = 0;
  if (utils::ToInt64(value, &integer)) {
    return CreateWithInt(integer);
  }
  if (value.size() <= EmbeddedString::kCapacity) {
    return CreateEmbedded(value);
  }
  return Create<std::string>(std::move(value));
}

std::unique_ptr<RedisObject> RedisObject::CreateWithStringCopy(
    std::string_view value) {
  int64_t integer = 0;
  if (utils::ToInt64(value, &integer)) {
    return CreateWithInt(integer);
  }
  if (value.size() <= EmbeddedString::kCapacity) {
    return CreateEmbedded(value);
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
  const auto* value = std::get_if<std::string>(&value_);
  RS_CHECK(value != nullptr);
  return StringValue(std::string_view(*value));
}

std::optional<int64_t> RedisObject::Int() const {
  if (const int64_t* integer = std::get_if<int64_t>(&value_)) {
    return *integer;
  }
  int64_t parsed = 0;
  return utils::ToInt64(String().View(), &parsed)
             ? std::optional<int64_t>(parsed)
             : std::nullopt;
}

std::string* RedisObject::MutableString() {
  if (const int64_t* integer = std::get_if<int64_t>(&value_)) {
    value_.emplace<std::string>(StringValue(*integer).View());
  } else if (const redis_simple::db::EmbeddedString* embedded =
                 std::get_if<EmbeddedString>(&value_)) {
    // Copy out first: emplacing destroys the embedded bytes.
    std::string raw(embedded->View());
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
  int64_t integer = 0;
  if (utils::ToInt64(value, &integer)) {
    value_.emplace<int64_t>(integer);
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
  auto* const current = std::get_if<std::string>(&value_);
  // An existing raw buffer is reused when it is small or well filled, which
  // overwrites without allocating. Otherwise a short value is embedded, which
  // also releases an oversized buffer.
  if (current != nullptr && (current->capacity() <= kAlwaysReusableBytes ||
                             current->capacity() / 2 <= value.size())) {
    current->assign(value);
    return;
  }
  if (value.size() <= EmbeddedString::kCapacity) {
    EmbedString(value);
    return;
  }
  if (current == nullptr) {
    value_.emplace<std::string>(value);
    return;
  }
  // Assigning, or emplacing the same alternative, would copy into the
  // oversized buffer and keep it; swapping in a new string frees it.
  std::string fresh(value);
  current->swap(fresh);
}

RedisObject::Encoding RedisObject::StringEncoding() const {
  if (std::holds_alternative<int64_t>(value_)) {
    return Encoding::kInt;
  }
  if (std::holds_alternative<EmbeddedString>(value_)) {
    return Encoding::kEmbedded;
  }
  return std::holds_alternative<std::string>(value_) ? Encoding::kRaw
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

}  // namespace redis_simple::db
