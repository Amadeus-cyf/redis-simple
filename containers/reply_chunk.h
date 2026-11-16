#ifndef REDIS_SIMPLE_CONTAINERS_REPLY_CHUNK_H_
#define REDIS_SIMPLE_CONTAINERS_REPLY_CHUNK_H_

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace redis_simple::containers {
// One chunk of a reply list: writable fixed-size storage, a moved std::string,
// or a string shared with its owner. The last two are sent without copying.
struct ReplyChunk {
  static std::unique_ptr<ReplyChunk> CreateWritable(size_t capacity) {
    auto node = std::make_unique<ReplyChunk>();
    // Bytes are written before they are read, so skip zero-filling.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
    node->storage.reset(new char[capacity]);
    node->capacity = capacity;
    return node;
  }
  static std::unique_ptr<ReplyChunk> CreateOwned(std::string data) {
    auto node = std::make_unique<ReplyChunk>();
    node->used = data.size();
    node->capacity = data.capacity();
    node->owned = std::move(data);
    return node;
  }

  // Holds `data`, which must not be null, for as long as the chunk lives, so
  // the bytes stay valid whatever the other holders do with their pointers.
  static std::unique_ptr<ReplyChunk> CreateShared(
      std::shared_ptr<const std::string> data) {
    auto node = std::make_unique<ReplyChunk>();
    node->used = data->size();
    node->capacity = data->size();
    node->shared = std::move(data);
    return node;
  }

  char* Data() { return storage != nullptr ? storage.get() : owned.data(); }
  // The bytes to send, for every kind of chunk.
  const char* Bytes() const {
    if (shared != nullptr) {
      return shared->data();
    }
    return storage != nullptr ? storage.get() : owned.data();
  }
  bool Writable() const { return storage != nullptr; }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
  std::unique_ptr<char[]> storage;
  std::string owned;
  std::shared_ptr<const std::string> shared;
  size_t used{};
  size_t capacity{};
  std::unique_ptr<ReplyChunk> next;
};
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_REPLY_CHUNK_H_
