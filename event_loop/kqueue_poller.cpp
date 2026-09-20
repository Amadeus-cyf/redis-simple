#include "event_loop/kqueue_poller.h"

#include <fcntl.h>
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "event_loop/loop.h"
#include "logging/logger.h"

namespace redis_simple::event_loop {
namespace {
int UpdateFilter(int kqueue_fd, int fd, int16_t filter, uint16_t flags) {
  struct kevent event{};
  EV_SET(&event, fd, filter, flags, 0, 0, nullptr);
  return kevent(kqueue_fd, &event, 1, nullptr, 0, nullptr);
}
}  // namespace

KqueuePoller::KqueuePoller(int fd, int nevents)
    : kqueue_fd_(fd), nevents_(nevents), events_(nevents) {
  ready_events_.reserve(static_cast<size_t>(nevents));
}

std::unique_ptr<KqueuePoller> KqueuePoller::Create(int nevents) {
  if (nevents <= 0) {
    return nullptr;
  }
  int kqueue_fd = kqueue();
  if (kqueue_fd < 0) {
    return nullptr;
  }
  const int descriptor_flags = fcntl(kqueue_fd, F_GETFD);
  if (descriptor_flags < 0 ||
      fcntl(kqueue_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
    close(kqueue_fd);
    return nullptr;
  }
  return std::unique_ptr<KqueuePoller>(new KqueuePoller(kqueue_fd, nevents));
}

int KqueuePoller::AddEvent(int fd, int mask) {
  RS_LOG_DEBUG("add api event for fd = %d, mask = %d\n", fd, mask);
  const auto existing = masks_.find(fd);
  const int existing_mask = existing == masks_.end() ? 0 : existing->second;
  const int added_mask = mask & ~existing_mask;
  if ((added_mask & EventFlag::kReadable) != 0) {
    RS_LOG_DEBUG("kqueue add read event\n");
    if (UpdateFilter(kqueue_fd_, fd, EVFILT_READ, EV_ADD) < 0) {
      RS_LOG_DEBUG("kevent add read event failed with errno: %d\n", errno);
      return -1;
    }
  }
  if ((added_mask & EventFlag::kWritable) != 0) {
    RS_LOG_DEBUG("kqueue add write event\n");
    if (UpdateFilter(kqueue_fd_, fd, EVFILT_WRITE, EV_ADD) < 0) {
      const int error = errno;
      if ((added_mask & EventFlag::kReadable) != 0) {
        UpdateFilter(kqueue_fd_, fd, EVFILT_READ, EV_DELETE);
      }
      RS_LOG_DEBUG("kevent add write event failed: %s\n", std::strerror(error));
      return -1;
    }
  }
  masks_[fd] = existing_mask | mask;
  RS_LOG_DEBUG("add event success\n");
  return 0;
}

int KqueuePoller::DeleteEvent(int fd, int mask) {
  const auto existing = masks_.find(fd);
  if (existing == masks_.end()) {
    return -1;
  }
  const int removed_mask = existing->second & mask;
  if ((removed_mask & EventFlag::kReadable) != 0) {
    if (UpdateFilter(kqueue_fd_, fd, EVFILT_READ, EV_DELETE) < 0) {
      RS_LOG_DEBUG("kevent delete read event failed: %s\n",
                   std::strerror(errno));
      return -1;
    }
  }
  if ((removed_mask & EventFlag::kWritable) != 0) {
    if (UpdateFilter(kqueue_fd_, fd, EVFILT_WRITE, EV_DELETE) < 0) {
      const int error = errno;
      if ((removed_mask & EventFlag::kReadable) != 0) {
        UpdateFilter(kqueue_fd_, fd, EVFILT_READ, EV_ADD);
      }
      RS_LOG_DEBUG("kevent delete write event failed: %s\n",
                   std::strerror(error));
      return -1;
    }
  }
  const int remaining_mask = existing->second & ~removed_mask;
  if (remaining_mask == 0) {
    masks_.erase(existing);
  } else {
    existing->second = remaining_mask;
  }
  return 0;
}

const std::vector<ReadyEvent>& KqueuePoller::Poll(
    struct timespec* timeout_spec) {
  ready_events_.clear();
  int numevents =
      kevent(kqueue_fd_, nullptr, 0, events_.data(), nevents_, timeout_spec);
  if (numevents < 0) {
    if (errno != EINTR) {
      RS_LOG_DEBUG("kevent poll failed: %s\n", std::strerror(errno));
    }
    return ready_events_;
  }
  RS_LOG_DEBUG("kqueue: poll %d fds from kqueue\n", numevents);
  for (int i = 0; i < numevents; i++) {
    if (events_[i].ident >
        static_cast<uintptr_t>(std::numeric_limits<int>::max())) {
      continue;
    }
    const int fd = static_cast<int>(events_[i].ident);
    int mask = 0;
    if (events_[i].filter == EVFILT_READ) {
      RS_LOG_DEBUG("kqueue: poll read\n");
      mask |= EventFlag::kReadable;
    }
    if (events_[i].filter == EVFILT_WRITE) {
      RS_LOG_DEBUG("kqueue: poll write\n");
      mask |= EventFlag::kWritable;
    }
    if (mask != 0) {
      ready_events_.push_back({fd, mask});
    }
  }
  std::sort(ready_events_.begin(), ready_events_.end(),
            [](const ReadyEvent& left, const ReadyEvent& right) {
              return left.fd < right.fd;
            });
  size_t output = 0;
  for (const auto& event : ready_events_) {
    if (output > 0 && ready_events_[output - 1].fd == event.fd) {
      ready_events_[output - 1].mask |= event.mask;
    } else {
      ready_events_[output++] = event;
    }
  }
  ready_events_.resize(output);
  return ready_events_;
}

KqueuePoller::~KqueuePoller() { close(kqueue_fd_); }
}  // namespace redis_simple::event_loop
