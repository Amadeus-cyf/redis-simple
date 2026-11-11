#ifndef REDIS_SIMPLE_UTILS_CRC64_H_
#define REDIS_SIMPLE_UTILS_CRC64_H_

#include <cstdint>
#include <string_view>

namespace redis_simple::utils {
// Extends `crc` over `data` with Redis' crc64 (CRC-64/Jones: the reflected
// polynomial 0xad93d23594c935a9, initial value 0, no final XOR), which RDB
// files end with. Start from 0; "123456789" gives 0xe9c6d914c4b8d9ca.
uint64_t Crc64(uint64_t crc, std::string_view data);
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_CRC64_H_
