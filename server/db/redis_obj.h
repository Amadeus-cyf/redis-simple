#ifndef REDIS_SIMPLE_SERVER_DB_REDIS_OBJ_H_
#define REDIS_SIMPLE_SERVER_DB_REDIS_OBJ_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "data_types/hash/hash.h"
#include "data_types/list/list.h"
#include "data_types/set/set.h"
#include "data_types/zset/zset.h"

namespace redis_simple::db {
// Read-only view of a string object's bytes. Integer-encoded strings are
// formatted into an inline buffer, so the view is safe to copy and needs no
// allocation. It stays valid until the object is mutated or destroyed.
class StringValue {
 public:
  explicit StringValue(std::string_view view)
      : external_(view.data()), size_(view.size()) {}
  explicit StringValue(int64_t value);

  std::string_view View() const {
    return external_ != nullptr ? std::string_view(external_, size_)
                                : std::string_view(buffer_.data(), size_);
  }
  // NOLINTNEXTLINE(google-explicit-constructor)
  operator std::string_view() const { return View(); }
  size_t size() const { return size_; }

  friend bool operator==(const StringValue& left, std::string_view right) {
    return left.View() == right;
  }
  friend bool operator!=(const StringValue& left, std::string_view right) {
    return left.View() != right;
  }
  friend std::ostream& operator<<(std::ostream& stream,
                                  const StringValue& value) {
    return stream << value.View();
  }

 private:
  // Long enough for "-9223372036854775808" plus a terminator.
  static constexpr size_t kInt64BufferSize = 21;
  const char* external_{nullptr};
  size_t size_{};
  std::array<char, kInt64BufferSize> buffer_{};
};

class RedisObject {
 private:
  using SetPtr = std::unique_ptr<set::Set>;
  using ListPtr = std::unique_ptr<list::List>;
  using ZSetPtr = std::unique_ptr<zset::ZSet>;
  using HashPtr = std::unique_ptr<hash::Hash>;
  // Integer-encoded strings are last so the other alternatives keep indices
  // that map directly onto ObjectType.
  using Value =
      std::variant<std::string, SetPtr, ListPtr, ZSetPtr, HashPtr, int64_t>;

 public:
  enum class ObjectType {
    kString = 1,
    kSet = 2,
    kList = 3,
    kZSet = 4,
    kHash = 5,
  };

  enum class Encoding {
    kRaw,
    kInt,
    kOther,
  };

  // Stores canonical decimal integers (such as "42" but not "042" or "+42")
  // as int64_t so counters avoid parsing and formatting on every update.
  static std::unique_ptr<RedisObject> CreateWithString(std::string value);
  static std::unique_ptr<RedisObject> CreateWithInt(int64_t value) {
    return Create(Value(value));
  }
  static std::unique_ptr<RedisObject> CreateWithSet(
      std::unique_ptr<set::Set> set) {
    return set == nullptr ? nullptr : Create(Value(std::move(set)));
  }
  static std::unique_ptr<RedisObject> CreateWithList(
      std::unique_ptr<list::List> list) {
    return list == nullptr ? nullptr : Create(Value(std::move(list)));
  }
  static std::unique_ptr<RedisObject> CreateWithZSet(
      std::unique_ptr<zset::ZSet> zset) {
    return zset == nullptr ? nullptr : Create(Value(std::move(zset)));
  }
  static std::unique_ptr<RedisObject> CreateWithHash(
      std::unique_ptr<hash::Hash> hash) {
    return hash == nullptr ? nullptr : Create(Value(std::move(hash)));
  }
  StringValue String() const;
  // Returns the integer value of a string object if it is integer-encoded or
  // holds a decimal integer.
  std::optional<int64_t> Int() const;
  // Converts an integer-encoded string to raw form so it can be edited.
  std::string* MutableString();
  // Replaces a string object's value with an integer-encoded one.
  void SetInt(int64_t value);
  Encoding StringEncoding() const;
  set::Set* Set();
  const set::Set* Set() const;
  list::List* List();
  const list::List* List() const;
  zset::ZSet* ZSet();
  const zset::ZSet* ZSet() const;
  hash::Hash* Hash();
  const hash::Hash* Hash() const;
  // ObjectType values follow the variant's alternative order, offset by one.
  ObjectType Type() const {
    if (std::holds_alternative<int64_t>(value_)) {
      return ObjectType::kString;
    }
    return static_cast<ObjectType>(value_.index() + 1);
  }

 private:
  static std::unique_ptr<RedisObject> Create(Value value) {
    return std::unique_ptr<RedisObject>(new RedisObject(std::move(value)));
  }
  explicit RedisObject(Value value) : value_(std::move(value)) {}

  template <ObjectType kType>
  using Alternative =
      std::variant_alternative_t<static_cast<size_t>(kType) - 1, Value>;
  static_assert(std::is_same_v<Alternative<ObjectType::kString>, std::string>);
  static_assert(std::is_same_v<Alternative<ObjectType::kSet>, SetPtr>);
  static_assert(std::is_same_v<Alternative<ObjectType::kList>, ListPtr>);
  static_assert(std::is_same_v<Alternative<ObjectType::kZSet>, ZSetPtr>);
  static_assert(std::is_same_v<Alternative<ObjectType::kHash>, HashPtr>);

  Value value_;
};

using RedisObjectPtr = std::unique_ptr<RedisObject>;
}  // namespace redis_simple::db

#endif  // REDIS_SIMPLE_SERVER_DB_REDIS_OBJ_H_
