#pragma once

#include <string_view>

#include "server/db/db.h"
#include "server/db/redis_obj.h"

namespace redis_simple::command {
// Result of looking up a key that must hold one object type. `object` is null
// when the key is missing or holds another type; `wrong_type` tells which.
template <typename Object>
struct TypedLookup {
  Object* object{nullptr};
  bool wrong_type{false};
};

inline TypedLookup<const db::RedisObject> LookupTyped(
    db::RedisDb* const db, std::string_view key,
    db::RedisObject::ObjectType type) {
  const db::RedisObject* const object = db->LookupKey(key);
  if (object == nullptr) {
    return {};
  }
  if (object->Type() != type) {
    return {nullptr, true};
  }
  return {object, false};
}

inline TypedLookup<db::RedisObject> MutableLookupTyped(
    db::RedisDb* const db, std::string_view key,
    db::RedisObject::ObjectType type) {
  db::RedisObject* const object = db->MutableLookupKey(key);
  if (object == nullptr) {
    return {};
  }
  if (object->Type() != type) {
    return {nullptr, true};
  }
  return {object, false};
}
}  // namespace redis_simple::command
