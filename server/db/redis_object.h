#ifndef REDIS_SIMPLE_SERVER_DB_REDIS_OBJECT_H_
#define REDIS_SIMPLE_SERVER_DB_REDIS_OBJECT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

// A short string stored inside its RedisObject, like Redis' embstr, so the
// value needs no allocation of its own. Its size matches std::string's plus
// one word, which grows each object by 8 bytes and moves values from
// std::string's inline limit (15 bytes in libstdc++) up to kCapacity bytes
// into the object's single allocation.
struct EmbeddedString {
  static constexpr size_t kCapacity = sizeof(std::string) + 7;

  // Copies only the value's bytes; the rest of `data` is never read.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  explicit EmbeddedString(std::string_view value)
      : size(static_cast<uint8_t>(value.size())) {
    std::memcpy(data.data(), value.data(), value.size());
  }

  std::string_view View() const { return {data.data(), size}; }

  std::array<char, kCapacity> data;
  uint8_t size;
};
static_assert(EmbeddedString::kCapacity <= UINT8_MAX);
static_assert(sizeof(EmbeddedString) == sizeof(std::string) + 8);

// A key's value and, for a key in the keyspace, its expiration time.
//
// The value is one of eight encodings held in the same bytes: a raw, shared,
// integer or embedded string, or a collection. Collections are held by value,
// inside the object, so a small one is not a separate allocation: each fits in
// the space an embedded string takes. Which encoding is live is a 3-bit tag in
// the header word, where Redis' robj packs its type and encoding into bit
// fields; a std::variant would spend a whole word on it.
//
// The rest of the header word is the expiration time, as Redis 8's kvobj
// keeps a key's TTL in the object. Only the keyspace uses it.
class RedisObject {
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
    kEmbedded,
    kOther,
  };

  // A string value of at least this many bytes, the size of a reply block,
  // is stored behind a shared pointer, so a reply can hold the bytes instead
  // of copying them.
  static constexpr size_t kMinSharedStringBytes = size_t{16} * 1024;
  // The largest expiration time the header word holds, about 73 million
  // years after the epoch in milliseconds.
  static constexpr int64_t kMaxExpire = (int64_t{1} << 61) - 1;

  // An empty raw string, the state of a keyspace entry's object before the
  // database stores a value in it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  RedisObject() { Construct<std::string>(); }
  // Moves the value and copies the expiration time.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  RedisObject(RedisObject&& other) noexcept : header_(other.header_) {
    MoveValueFrom(&other);
  }
  // Replaces the value and keeps this object's expiration time, so storing
  // a new value in a key leaves the key's TTL to the database.
  RedisObject& operator=(RedisObject&& other) noexcept {
    if (this != &other) {
      DestroyValue();
      SetTag(other.CurrentTag());
      MoveValueFrom(&other);
    }
    return *this;
  }
  RedisObject(const RedisObject&) = delete;
  RedisObject& operator=(const RedisObject&) = delete;
  ~RedisObject() { DestroyValue(); }

  // Stores canonical decimal integers (such as "42" but not "042" or "+42")
  // as int64_t so counters avoid parsing and formatting on every update, and
  // other values of at most EmbeddedString::kCapacity bytes inline.
  static std::unique_ptr<RedisObject> CreateWithString(std::string value);
  // Like CreateWithString, but copies the value only when it does not fit
  // inline, so short values need no temporary std::string.
  static std::unique_ptr<RedisObject> CreateWithStringCopy(
      std::string_view value);
  static std::unique_ptr<RedisObject> CreateWithInt(int64_t value) {
    return Create<int64_t>(value);
  }
  // The collection is moved into the object.
  static std::unique_ptr<RedisObject> CreateWithSet(
      std::unique_ptr<data_types::Set> set) {
    return set == nullptr ? nullptr : Create<data_types::Set>(std::move(*set));
  }
  static std::unique_ptr<RedisObject> CreateWithList(
      std::unique_ptr<data_types::List> list) {
    return list == nullptr ? nullptr
                           : Create<data_types::List>(std::move(*list));
  }
  static std::unique_ptr<RedisObject> CreateWithZSet(
      std::unique_ptr<data_types::ZSet> zset) {
    return zset == nullptr ? nullptr
                           : Create<data_types::ZSet>(std::move(*zset));
  }
  static std::unique_ptr<RedisObject> CreateWithHash(
      std::unique_ptr<data_types::Hash> hash) {
    return hash == nullptr ? nullptr
                           : Create<data_types::Hash>(std::move(*hash));
  }
  // String accessors require a string object (Type() == kString).
  StringValue String() const;
  // The bytes of a string stored behind a shared pointer, or nullptr for
  // any other string. The bytes stay as they are for as long as the caller
  // holds the pointer, on any thread: a write to the object, or its
  // destruction, leaves a buffer that someone else holds alone and gives the
  // object a new one. Call it on the main thread only.
  std::shared_ptr<const std::string> SharedStringBytes() const;
  // Returns the integer value if the string is integer-encoded or holds a
  // decimal integer.
  std::optional<int64_t> Int() const;
  // Converts an integer- or embedded-encoded string to raw form so it can be
  // edited.
  // Returns nullptr if this is not a string object.
  std::string* MutableString();
  // Replaces a string object's value with an integer-encoded one.
  void SetInt(int64_t value);
  // Replaces a string object's value with the encoding CreateWithString would
  // choose, reusing the existing buffer when the new value fills a fair share
  // of it so that shrinking a large value releases its memory.
  void AssignString(std::string_view value);
  // Replaces any value with a copy of `value`, in the encoding
  // CreateWithStringCopy would choose.
  void ReplaceWithString(std::string_view value);
  Encoding StringEncoding() const;
  // Collection accessors return nullptr if the object holds another type.
  data_types::Set* Set() { return GetIf<data_types::Set>(); }
  const data_types::Set* Set() const { return GetIf<data_types::Set>(); }
  data_types::List* List() { return GetIf<data_types::List>(); }
  const data_types::List* List() const { return GetIf<data_types::List>(); }
  data_types::ZSet* ZSet() { return GetIf<data_types::ZSet>(); }
  const data_types::ZSet* ZSet() const { return GetIf<data_types::ZSet>(); }
  data_types::Hash* Hash() { return GetIf<data_types::Hash>(); }
  const data_types::Hash* Hash() const { return GetIf<data_types::Hash>(); }
  // Frees up to about `max_elements` elements of a large collection, so a
  // discarded value can be freed in steps. Returns true once the rest is
  // cheap to free at once; until then, only destroy the object.
  bool ReleaseSome(size_t max_elements);
  ObjectType Type() const {
    const Tag tag = CurrentTag();
    // The tags from kInt on are other encodings of a string; the ones before
    // follow ObjectType's order, offset by one.
    return tag >= Tag::kInt
               ? ObjectType::kString
               : static_cast<ObjectType>(static_cast<uint8_t>(tag) + 1);
  }
  // The expiration time the database stored: an absolute time in
  // milliseconds from 1 to kMaxExpire, or 0 for none.
  int64_t Expire() const { return static_cast<int64_t>(header_ >> kTagBits); }
  void SetExpire(int64_t expire) {
    header_ =
        (static_cast<uint64_t>(expire) << kTagBits) | (header_ & kTagMask);
  }

 private:
  enum class Tag : uint8_t {
    kRaw,
    kSet,
    kList,
    kZSet,
    kHash,
    kInt,
    kEmbedded,
    kShared,
  };
  using SharedString = std::shared_ptr<std::string>;
  static constexpr int kTagBits = 3;
  static constexpr uint64_t kTagMask = (uint64_t{1} << kTagBits) - 1;
  static_assert(static_cast<uint64_t>(Tag::kShared) <= kTagMask);
  static_assert(static_cast<int>(Tag::kRaw) + 1 ==
                static_cast<int>(ObjectType::kString));
  static_assert(static_cast<int>(Tag::kSet) + 1 ==
                static_cast<int>(ObjectType::kSet));
  static_assert(static_cast<int>(Tag::kList) + 1 ==
                static_cast<int>(ObjectType::kList));
  static_assert(static_cast<int>(Tag::kZSet) + 1 ==
                static_cast<int>(ObjectType::kZSet));
  static_assert(static_cast<int>(Tag::kHash) + 1 ==
                static_cast<int>(ObjectType::kHash));

  template <typename T>
  static constexpr Tag TagOf() {
    if constexpr (std::is_same_v<T, std::string>) {
      return Tag::kRaw;
    } else if constexpr (std::is_same_v<T, data_types::Set>) {
      return Tag::kSet;
    } else if constexpr (std::is_same_v<T, data_types::List>) {
      return Tag::kList;
    } else if constexpr (std::is_same_v<T, data_types::ZSet>) {
      return Tag::kZSet;
    } else if constexpr (std::is_same_v<T, data_types::Hash>) {
      return Tag::kHash;
    } else if constexpr (std::is_same_v<T, int64_t>) {
      return Tag::kInt;
    } else if constexpr (std::is_same_v<T, SharedString>) {
      return Tag::kShared;
    } else {
      static_assert(std::is_same_v<T, EmbeddedString>);
      return Tag::kEmbedded;
    }
  }

  template <typename T, typename... Args>
  static std::unique_ptr<RedisObject> Create(Args&&... args) {
    return std::unique_ptr<RedisObject>(
        new RedisObject(std::in_place_type<T>, std::forward<Args>(args)...));
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  template <typename T, typename... Args>
  explicit RedisObject(std::in_place_type_t<T> /*type*/, Args&&... args) {
    Construct<T>(std::forward<Args>(args)...);
  }

  static std::unique_ptr<RedisObject> CreateEmbedded(std::string_view value);
  // The string the object may change in place: its raw string, or its shared
  // one while nothing else holds it. Null for the other encodings.
  std::string* ExclusiveString();
  // Stores a string too long to embed, shared when it is large enough.
  void EmplaceLongString(std::string value);
  // Replaces the value with an embedded copy of `value`, which must fit and
  // may view this object's current bytes.
  void EmbedString(std::string_view value);

  // The tag bits always hold a Tag: SetTag is all that writes them.
  Tag CurrentTag() const {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<Tag>(header_ & kTagMask);
  }
  void SetTag(Tag tag) {
    header_ = (header_ & ~kTagMask) | static_cast<uint64_t>(tag);
  }
  // The value as a T, which must be the live encoding.
  template <typename T>
  T* As() {
    return std::launder(reinterpret_cast<T*>(value_.data()));
  }
  template <typename T>
  const T* As() const {
    return std::launder(reinterpret_cast<const T*>(value_.data()));
  }
  // The value if its encoding is T, else nullptr.
  template <typename T>
  T* GetIf() {
    return CurrentTag() == TagOf<T>() ? As<T>() : nullptr;
  }
  template <typename T>
  const T* GetIf() const {
    return CurrentTag() == TagOf<T>() ? As<T>() : nullptr;
  }
  // Builds a T in the value's bytes, which must hold no live value.
  template <typename T, typename... Args>
  void Construct(Args&&... args) {
    static_assert(sizeof(T) <= kValueBytes && alignof(T) <= kValueAlignment);
    ::new (static_cast<void*>(value_.data())) T(std::forward<Args>(args)...);
    SetTag(TagOf<T>());
  }
  // Replaces the value. The arguments must not refer to the current value,
  // which is destroyed first.
  template <typename T, typename... Args>
  void Emplace(Args&&... args) {
    DestroyValue();
    Construct<T>(std::forward<Args>(args)...);
  }
  // Calls `visitor` with the live value, as its own type.
  template <typename Visitor>
  void VisitValue(Visitor&& visitor) {
    switch (CurrentTag()) {
      case Tag::kRaw:
        return visitor(*As<std::string>());
      case Tag::kSet:
        return visitor(*As<data_types::Set>());
      case Tag::kList:
        return visitor(*As<data_types::List>());
      case Tag::kZSet:
        return visitor(*As<data_types::ZSet>());
      case Tag::kHash:
        return visitor(*As<data_types::Hash>());
      case Tag::kInt:
        return visitor(*As<int64_t>());
      case Tag::kEmbedded:
        return visitor(*As<EmbeddedString>());
      case Tag::kShared:
        return visitor(*As<SharedString>());
    }
  }
  void DestroyValue() {
    VisitValue([](auto& value) { std::destroy_at(&value); });
  }
  // Builds this object's value from `other`'s, whose tag this object already
  // has. `other` keeps a moved-from value of the same encoding.
  void MoveValueFrom(RedisObject* other) {
    other->VisitValue([this](auto& value) {
      Construct<std::remove_reference_t<decltype(value)>>(std::move(value));
    });
  }

  // An embedded string is the largest encoding; the static_asserts after
  // the class check the collections against it.
  static constexpr size_t kValueBytes = sizeof(EmbeddedString);
  static constexpr size_t kValueAlignment = alignof(uint64_t);

  alignas(kValueAlignment) std::array<std::byte, kValueBytes> value_;
  // The tag in the low kTagBits bits, the expiration time above them.
  uint64_t header_{};
};

// Holding collections did not make the object larger than an embedded string
// already did. The collections are checked one by one, so a failure here
// names the one that grew: std::string, and so EmbeddedString, is smaller
// with libc++ (macOS) than with libstdc++.
static_assert(sizeof(data_types::Set) <= sizeof(EmbeddedString));
static_assert(sizeof(data_types::List) <= sizeof(EmbeddedString));
static_assert(sizeof(data_types::ZSet) <= sizeof(EmbeddedString));
static_assert(sizeof(data_types::Hash) <= sizeof(EmbeddedString));
static_assert(sizeof(RedisObject) == sizeof(EmbeddedString) + 8);

using RedisObjectPtr = std::unique_ptr<RedisObject>;
// The type's name, as TYPE replies with it and SCAN's TYPE option matches it.
std::string_view TypeName(RedisObject::ObjectType type);
// The type with that name, ignoring case; nullopt for any other name.
std::optional<RedisObject::ObjectType> TypeFromName(std::string_view name);
}  // namespace redis_simple::db

#endif  // REDIS_SIMPLE_SERVER_DB_REDIS_OBJECT_H_
