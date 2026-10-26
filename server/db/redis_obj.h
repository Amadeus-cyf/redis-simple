#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "data_types/hash/hash.h"
#include "data_types/list/list.h"
#include "data_types/set/set.h"
#include "data_types/zset/zset.h"

namespace redis_simple::db {
class RedisObject {
 private:
  using SetPtr = std::unique_ptr<set::Set>;
  using ListPtr = std::unique_ptr<list::List>;
  using ZSetPtr = std::unique_ptr<zset::ZSet>;
  using HashPtr = std::unique_ptr<hash::Hash>;
  using Value = std::variant<std::string, SetPtr, ListPtr, ZSetPtr, HashPtr>;

 public:
  enum class ObjectType {
    kString = 1,
    kSet = 2,
    kList = 3,
    kZSet = 4,
    kHash = 5,
  };

  static std::unique_ptr<RedisObject> CreateWithString(std::string value) {
    return Create(Value(std::move(value)));
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
  const std::string& String() const;
  std::string* MutableString();
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
