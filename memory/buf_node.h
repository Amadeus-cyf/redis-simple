#ifndef REDIS_SIMPLE_MEMORY_BUF_NODE_H_
#define REDIS_SIMPLE_MEMORY_BUF_NODE_H_

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace redis_simple::in_memory {
// One chunk of a reply list: either writable fixed-size storage or a moved
// std::string that is sent without copying.
struct BufNode {
  static std::unique_ptr<BufNode> CreateWritable(size_t capacity) {
    auto node = std::make_unique<BufNode>();
    // Bytes are written before they are read, so skip zero-filling.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
    node->storage.reset(new char[capacity]);
    node->capacity = capacity;
    return node;
  }
  static std::unique_ptr<BufNode> CreateOwned(std::string data) {
    auto node = std::make_unique<BufNode>();
    node->used = data.size();
    node->capacity = data.capacity();
    node->owned = std::move(data);
    return node;
  }

  char* Data() { return storage != nullptr ? storage.get() : owned.data(); }
  bool Writable() const { return storage != nullptr; }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays): contiguous byte buffer
  std::unique_ptr<char[]> storage;
  std::string owned;
  size_t used{};
  size_t capacity{};
  std::unique_ptr<BufNode> next;
};
}  // namespace redis_simple::in_memory

#endif  // REDIS_SIMPLE_MEMORY_BUF_NODE_H_
