#ifndef REDIS_SIMPLE_SERVER_VERSION_H_
#define REDIS_SIMPLE_SERVER_VERSION_H_

#include <string_view>

namespace redis_simple {
// The version HELLO reports, and RDB files record as "redis-ver".
inline constexpr std::string_view kServerVersion = "0.1.0";
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_VERSION_H_
