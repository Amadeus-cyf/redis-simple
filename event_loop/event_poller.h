#ifndef REDIS_SIMPLE_EVENT_LOOP_EVENT_POLLER_H_
#define REDIS_SIMPLE_EVENT_LOOP_EVENT_POLLER_H_

#include <ctime>
#include <vector>

namespace redis_simple::event_loop {
// A descriptor and the EventFlag bits (kReadable, kWritable) ready on it.
struct ReadyEvent {
  int fd;
  int mask;
};

// The operating system's readiness notification (kqueue or epoll).
class EventPoller {
 public:
  // Start or stop watching fd for the EventFlag bits in `mask`, keeping its
  // other registered bits. Both return 0, or -1 on failure.
  virtual int AddEvent(int fd, int mask) = 0;
  virtual int DeleteEvent(int fd, int mask) = 0;
  // Waits up to timeout_spec (forever if null) and returns one entry per
  // ready descriptor, empty on timeout or error. The result is valid until
  // the next call.
  virtual const std::vector<ReadyEvent>& Poll(
      struct timespec* timeout_spec) = 0;
  virtual ~EventPoller() = default;
};
}  // namespace redis_simple::event_loop

#endif  // REDIS_SIMPLE_EVENT_LOOP_EVENT_POLLER_H_
