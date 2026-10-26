#include "redis_obj.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "utils/check.h"
#include "utils/string_utils.h"

namespace redis_simple::db {
StringValue::StringValue(int64_t value) {
  const int length =
      utils::Int64ToString(buffer_.data(), buffer_.size(), value);
  size_ = static_cast<size_t>(length);
}

std::unique_ptr<RedisObject> RedisObject::CreateWithString(std::string value) {
  // ToCanonicalInt64 rejects forms like "+1" and "007" so GET returns the
  // exact bytes SET stored.
  int64_t integer = 0;
  if (utils::ToCanonicalInt64(value, &integer)) {
    return CreateWithInt(integer);
  }
  return Create(Value(std::move(value)));
}

StringValue RedisObject::String() const {
  if (const auto* integer = std::get_if<int64_t>(&value_)) {
    return StringValue(*integer);
  }
  const auto* value = std::get_if<std::string>(&value_);
  RS_CHECK(value != nullptr);
  return StringValue(std::string_view(*value));
}

std::optional<int64_t> RedisObject::Int() const {
  if (const auto* integer = std::get_if<int64_t>(&value_)) {
    return *integer;
  }
  const auto* value = std::get_if<std::string>(&value_);
  RS_CHECK(value != nullptr);
  int64_t parsed = 0;
  return utils::ToInt64(*value, &parsed) ? std::optional<int64_t>(parsed)
                                         : std::nullopt;
}

std::string* RedisObject::MutableString() {
  if (const auto* integer = std::get_if<int64_t>(&value_)) {
    value_ = std::string(StringValue(*integer).View());
  }
  return std::get_if<std::string>(&value_);
}

void RedisObject::SetInt(int64_t value) {
  RS_CHECK(Type() == ObjectType::kString);
  value_ = value;
}

RedisObject::Encoding RedisObject::StringEncoding() const {
  if (std::holds_alternative<int64_t>(value_)) {
    return Encoding::kInt;
  }
  return std::holds_alternative<std::string>(value_) ? Encoding::kRaw
                                                     : Encoding::kOther;
}
}  // namespace redis_simple::db
