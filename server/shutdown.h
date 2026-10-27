#ifndef REDIS_SIMPLE_SERVER_SHUTDOWN_H_
#define REDIS_SIMPLE_SERVER_SHUTDOWN_H_

namespace redis_simple::shutdown {
bool InstallSignalHandlers();
bool StopRequested();
}  // namespace redis_simple::shutdown

#endif  // REDIS_SIMPLE_SERVER_SHUTDOWN_H_
