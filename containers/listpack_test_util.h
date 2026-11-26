#ifndef REDIS_SIMPLE_CONTAINERS_LISTPACK_TEST_UTIL_H_
#define REDIS_SIMPLE_CONTAINERS_LISTPACK_TEST_UTIL_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace redis_simple::containers::listpack_test {
// The bytes of a hash listpack holding the field "f" and a value of `length`
// bytes of `fill`, laid out as Redis does for HSET h f <value> with a raised
// hash-max-listpack-value. The value is a string with a 32-bit length, so
// its encoded size is `length` + 5. `backlen` is written after it as given,
// so a test can pass the usual width, the short one, or a corrupt one.
inline std::string HashListpack(size_t length, char fill,
                                std::string_view backlen) {
  // "f" as a string with a 6-bit length (81 66), its backlen (02), and the
  // value's encoding byte (f0), which its length follows, least significant
  // byte first.
  std::string body("\x81\x66\x02\xf0", 4);
  for (int shift = 0; shift < 32; shift += 8) {
    body.push_back(static_cast<char>((length >> shift) & 0xff));
  }
  body.append(length, fill);
  body.append(backlen);
  // The header's 6 bytes hold the total size and the element count, 2. The
  // terminator is the last byte.
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
