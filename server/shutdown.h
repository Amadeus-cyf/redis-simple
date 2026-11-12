#ifndef REDIS_SIMPLE_SERVER_SHUTDOWN_H_
#define REDIS_SIMPLE_SERVER_SHUTDOWN_H_

namespace redis_simple::shutdown {
bool InstallSignalHandlers();
bool StopRequested();
// Forgets a stop request the server could not carry out.
void ClearStopRequest();
}  // namespace redis_simple::shutdown

#endif  // REDIS_SIMPLE_SERVER_SHUTDOWN_H_
