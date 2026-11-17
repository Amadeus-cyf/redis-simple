#ifndef REDIS_SIMPLE_SERVER_SHUTDOWN_H_
#define REDIS_SIMPLE_SERVER_SHUTDOWN_H_

namespace redis_simple::shutdown {
// Makes SIGINT and SIGTERM request a stop, ignores SIGPIPE so that writing
// to a closed socket fails with an error instead of ending the process, and
// forgets any earlier stop request. Returns false if a handler could not be
// installed.
bool InstallSignalHandlers();
bool StopRequested();
// Forgets a stop request the server could not carry out.
void ClearStopRequest();
}  // namespace redis_simple::shutdown

#endif  // REDIS_SIMPLE_SERVER_SHUTDOWN_H_
