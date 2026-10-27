#ifndef REDIS_SIMPLE_CONNECTION_CONNECTION_CALLBACK_H_
#define REDIS_SIMPLE_CONNECTION_CONNECTION_CALLBACK_H_

#include <functional>

namespace redis_simple::connection {
class Connection;

using ConnectionCallback = std::function<void(Connection*)>;
}  // namespace redis_simple::connection

#endif  // REDIS_SIMPLE_CONNECTION_CONNECTION_CALLBACK_H_
