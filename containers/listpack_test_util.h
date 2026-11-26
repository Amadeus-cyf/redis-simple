#ifndef REDIS_SIMPLE_CONTAINERS_LISTPACK_TEST_UTIL_H_
#define REDIS_SIMPLE_CONTAINERS_LISTPACK_TEST_UTIL_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace redis_simple::containers::listpack_test {
// A listpack Redis wrote for HSET h f <value> with a raised
// hash-max-listpack-value, where `value` is a 32-bit-length string of
// `length` bytes of `fill`, so its encoded size is `length` + 5, followed by
// the value's backlen `backlen`.
inline std::string HashListpack(size_t length, char fill,
                                std::string_view backlen) {
  std::string body("\x81\x66\x02\xf0", 4);
  for (int shift = 0; shift < 32; shift += 8) {
    body.push_back(static_cast<char>((length >> shift) & 0xff));
  }
  body.append(length, fill);
  body.append(backlen);
  const size_t total = 6 + body.size() + 1;
  std::string listpack;
  for (int shift = 0; shift < 32; shift += 8) {
    listpack.push_back(static_cast<char>((total >> shift) & 0xff));
  }
  listpack.append("\x02\x00", 2);
  return listpack + body + '\xff';
}
}  // namespace redis_simple::containers::listpack_test

#endif  // REDIS_SIMPLE_CONTAINERS_LISTPACK_TEST_UTIL_H_
