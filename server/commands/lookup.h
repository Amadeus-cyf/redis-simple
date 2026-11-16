#ifndef REDIS_SIMPLE_SERVER_COMMANDS_LOOKUP_H_
#define REDIS_SIMPLE_SERVER_COMMANDS_LOOKUP_H_

#include <string_view>

#include "server/db/db.h"
#include "server/db/redis_object.h"

namespace redis_simple::commands {
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

// Result of LookupOrCreateTyped. `object` is null only when the key holds
// another type.
struct CreatingLookup {
  db::RedisObject* object{nullptr};
  bool created{false};
  bool wrong_type{false};
};

// MutableLookupTyped that adds a missing key, hashing the key and probing
// the keyspace once for both (RedisDb::FindOrCreateKey). A created key holds
// an empty string: the caller stores its value of `type` before returning.
inline CreatingLookup LookupOrCreateTyped(db::RedisDb* const db,
                                          std::string_view key,
                                          db::RedisObject::ObjectType type) {
  const db::RedisDb::KeySlot slot = db->FindOrCreateKey(key);
  if (slot.Created()) {
    return {.object = slot.Object(), .created = true};
  }
  if (slot.Object()->Type() != type) {
    return {.wrong_type = true};
  }
  return {.object = slot.Object()};
}
}  // namespace redis_simple::commands

#endif  // REDIS_SIMPLE_SERVER_COMMANDS_LOOKUP_H_
