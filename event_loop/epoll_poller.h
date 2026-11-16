#ifndef REDIS_SIMPLE_EVENT_LOOP_EPOLL_POLLER_H_
#define REDIS_SIMPLE_EVENT_LOOP_EPOLL_POLLER_H_

#include <sys/epoll.h>

#include <memory>
#include <vector>

#include "event_loop/event_poller.h"

namespace redis_simple::event_loop {
class EpollPoller final : public EventPoller {
 public:
  static std::unique_ptr<EpollPoller> Create(int nevents);
  int AddEvent(int fd, int mask) override;
  int DeleteEvent(int fd, int mask) override;
  const std::vector<ReadyEvent>& Poll(struct timespec* timeout_spec) override;
  ~EpollPoller() override;

 private:
  explicit EpollPoller(int fd, int nevents);
  int epoll_fd_;
  int nevents_;
  std::vector<struct epoll_event> events_;
  std::vector<ReadyEvent> ready_events_;
  // The registered mask of each descriptor, indexed by descriptor; 0 when
  // none. Descriptors are small and dense, so this needs no hashing.
  std::vector<int> masks_;
};
}  // namespace redis_simple::event_loop

#endif  // REDIS_SIMPLE_EVENT_LOOP_EPOLL_POLLER_H_
