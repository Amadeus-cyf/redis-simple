#ifndef REDIS_SIMPLE_CONTAINERS_INTSET_TEST_UTIL_H_
#define REDIS_SIMPLE_CONTAINERS_INTSET_TEST_UTIL_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::containers::intset_test {
// An intset in Redis' layout whose header gives `width` and `length`, each
// as a 32-bit integer, followed by `values` as they are. A test can pass a
// header that does not match the values.
inline std::string RawBlob(uint32_t width, uint32_t length,
                           std::string_view values) {
  std::string blob;
  for (const uint32_t word : {width, length}) {
    for (int shift = 0; shift < 32; shift += 8) {
      blob.push_back(static_cast<char>((word >> shift) & 0xff));
    }
  }
  blob.append(values);
  return blob;
}

// The blob of `values` stored in the order given at `width` bytes each, which
// is 2, 4 or 8, least significant byte first.
inline std::string Blob(uint32_t width, const std::vector<int64_t>& values) {
  std::string bytes;
  for (const int64_t value : values) {
    for (uint32_t i = 0; i < width; ++i) {
      bytes.push_back(
          static_cast<char>((static_cast<uint64_t>(value) >> (8 * i)) & 0xff));
    }
  }
  return RawBlob(width, static_cast<uint32_t>(values.size()), bytes);
}
}  // namespace redis_simple::containers::intset_test

#endif  // REDIS_SIMPLE_CONTAINERS_INTSET_TEST_UTIL_H_
