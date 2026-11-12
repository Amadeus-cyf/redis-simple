#include "server/shutdown.h"

#include <csignal>

namespace redis_simple::shutdown {
namespace {
// Signal handlers may only update signal-safe state.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t stop_requested = 0;

void RequestStop(int /*signal*/) { stop_requested = 1; }
}  // namespace

bool InstallSignalHandlers() {
  stop_requested = 0;
  return std::signal(SIGINT, RequestStop) != SIG_ERR &&
         std::signal(SIGTERM, RequestStop) != SIG_ERR &&
         std::signal(SIGPIPE, SIG_IGN) != SIG_ERR;
}

bool StopRequested() { return stop_requested != 0; }

void ClearStopRequest() { stop_requested = 0; }
}  // namespace redis_simple::shutdown
