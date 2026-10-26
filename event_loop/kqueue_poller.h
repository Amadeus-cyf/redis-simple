#ifndef REDIS_SIMPLE_EVENT_LOOP_KQUEUE_POLLER_H_
#define REDIS_SIMPLE_EVENT_LOOP_KQUEUE_POLLER_H_

#include <sys/event.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "event_loop/event_poller.h"

namespace redis_simple::event_loop {
class KqueuePoller final : public EventPoller {
 public:
  static std::unique_ptr<KqueuePoller> Create(int nevents);
  int AddEvent(int fd, int mask) override;
  int DeleteEvent(int fd, int mask) override;
  const std::vector<ReadyEvent>& Poll(struct timespec* timeout_spec) override;
  ~KqueuePoller() override;

 private:
  explicit KqueuePoller(int fd, int nevents);
  int kqueue_fd_;
  int nevents_;
  std::vector<struct kevent> events_;
  std::vector<ReadyEvent> ready_events_;
  std::unordered_map<int, int> masks_;
};
}  // namespace redis_simple::event_loop

#endif  // REDIS_SIMPLE_EVENT_LOOP_KQUEUE_POLLER_H_
