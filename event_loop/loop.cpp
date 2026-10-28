#include "event_loop/loop.h"

#include <poll.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "utils/time_utils.h"

#ifdef __APPLE__
#include "event_loop/kqueue_poller.h"
#elifdef __linux__
#include "event_loop/epoll_poller.h"
#else
#error "Unsupported event loop platform"
#endif

namespace redis_simple::event_loop {
namespace {
constexpr int kIoEventMask =
    ToInt(EventFlag::kReadable) | ToInt(EventFlag::kWritable);
constexpr int64_t kDefaultPollTimeoutMs = 1000;
constexpr int64_t kMillisecondsPerSecond = 1000;
constexpr int64_t kNanosecondsPerMillisecond = 1000000;
}  // namespace

void FileEvent::Merge(const FileEvent* file_event) {
  if (file_event == nullptr) {
    return;
  }
  const bool merges_separate_single_purpose_callbacks =
      (HasReadCallback() && !HasWriteCallback() &&
       file_event->HasWriteCallback() && !file_event->HasReadCallback()) ||
      (HasWriteCallback() && !HasReadCallback() &&
       file_event->HasReadCallback() && !file_event->HasWriteCallback());
  AddMask(file_event->Mask());
  if (!HasReadCallback() && file_event->HasReadCallback()) {
    read_callback_ = file_event->read_callback_;
  }
  if (!HasWriteCallback() && file_event->HasWriteCallback()) {
    write_callback_ = file_event->write_callback_;
  }
  has_separate_callbacks_ =
      HasReadCallback() && HasWriteCallback() &&
      (has_separate_callbacks_ || file_event->has_separate_callbacks_ ||
       merges_separate_single_purpose_callbacks);
}

int WaitForEvent(int fd, int mask, int64_t timeout_ms) {
  constexpr nfds_t kFdCount = 1;
  timeout_ms = std::max<int64_t>(timeout_ms, -1);
  timeout_ms = std::min<int64_t>(timeout_ms, std::numeric_limits<int>::max());
  std::array<pollfd, 1> poll_fds{};
  poll_fds[0].fd = fd;
  if ((mask & EventFlag::kReadable) != 0) {
    poll_fds[0].events |= POLLIN;
  }
  if ((mask & EventFlag::kWritable) != 0) {
    poll_fds[0].events |= POLLOUT;
  }
  int r = -1;
  while (r < 0) {
    r = poll(poll_fds.data(), kFdCount, static_cast<int>(timeout_ms));
    if (r < 0 && errno != EINTR) {
      break;
    }
  }
  if (r < 0) {
    RS_LOG_DEBUG("poll error: %s\n", std::strerror(errno));
    return r;
  }
  if (r == 0) {
    // The file descriptor is not ready for the requested operation.
    return r;
  }
  if ((poll_fds[0].revents & POLLNVAL) != 0) {
    return -1;
  }
  int result_mask = 0;
  if ((poll_fds[0].revents & POLLIN) != 0) {
    result_mask |= EventFlag::kReadable;
  }
  if (((poll_fds[0].revents & POLLOUT) != 0) ||
      ((poll_fds[0].revents & POLLHUP) != 0) ||
      ((poll_fds[0].revents & POLLERR) != 0)) {
    result_mask |= EventFlag::kWritable;
  }
  return result_mask;
}

Loop::Loop(std::unique_ptr<EventPoller> event_poller)
    : event_poller_(std::move(event_poller)) {}

std::unique_ptr<Loop> Loop::Create() {
#ifdef __APPLE__
  auto event_poller = KqueuePoller::Create(kMaxReadyEvents);
#elifdef __linux__
  auto event_poller = EpollPoller::Create(kMaxReadyEvents);
#endif
  if (!event_poller) {
    return nullptr;
  }
  return std::unique_ptr<Loop>(new Loop(std::move(event_poller)));
}

void Loop::Run() {
  stop_requested_ = false;
  while (!stop_requested_) {
    ProcessEvents();
  }
}

Status Loop::CreateFileEvent(int fd, std::unique_ptr<FileEvent> file_event) {
  if (file_event == nullptr) {
    return Status::kError;
  }
  RS_LOG_DEBUG("create events for fd = %d, mask = %d\n", fd,
               file_event->Mask());
  if (fd < 0) {
    return Status::kError;
  }
  const int io_mask = file_event->Mask() & kIoEventMask;
  if (io_mask == 0) {
    return Status::kError;
  }
  const auto index = static_cast<size_t>(fd);
  if (index >= file_events_.size()) {
    file_events_.resize(index + 1);
  }
  if (event_poller_->AddEvent(fd, io_mask) < 0) {
    return Status::kError;
  }
  if (file_events_[index] == nullptr) {
    RS_LOG_DEBUG("add new event\n");
  } else {
    // A descriptor stores one FileEvent, so merge separately registered
    // read/write callbacks before replacing the old event.
    file_event->Merge(file_events_[index].get());
    if (processing_file_events_) {
      retired_file_events_.push_back(std::move(file_events_[index]));
    }
  }
  file_events_[index] = std::move(file_event);
  return Status::kOk;
}

Status Loop::DeleteFileEvent(int fd, int mask) {
  if (fd < 0 || static_cast<size_t>(fd) >= file_events_.size() ||
      file_events_[fd] == nullptr) {
    return Status::kError;
  }
  FileEvent* file_event = file_events_[fd].get();
  const int registered_io_mask = file_event->Mask() & kIoEventMask;
  const int removed_io_mask = registered_io_mask & mask;
  if (removed_io_mask == 0 ||
      event_poller_->DeleteEvent(fd, removed_io_mask) < 0) {
    RS_LOG_DEBUG(
        "fail to delete the file event of file descriptor %d with errno: "
        "%d\n",
        fd, errno);
    return Status::kError;
  }
  RS_LOG_DEBUG(
      "delete file event success for file descriptor = %d, mask = %d\n", fd,
      mask);
  if (registered_io_mask == removed_io_mask) {
    if (processing_file_events_) {
      retired_file_events_.push_back(std::move(file_events_[fd]));
    } else {
      file_events_[fd].reset();
    }
  } else {
    int m = file_event->Mask();
    m &= ~removed_io_mask;
    if ((m & EventFlag::kWritable) == 0) {
      m &= ~EventFlag::kBarrier;
    }
    file_event->SetMask(m);
  }
  return Status::kOk;
}

void Loop::CreateTimeEvent(std::unique_ptr<TimeEvent> time_event) {
  if (time_event != nullptr) {
    time_events_.push_front(std::move(time_event));
  }
}

void Loop::Defer(std::function<void()> callback) {
  if (callback) {
    deferred_callbacks_.push_back(std::move(callback));
  }
}

void Loop::ProcessEvents() {
  ProcessFileEvents();
  ProcessDeferredCallbacks();
  ProcessTimeEvents();
}

void Loop::ProcessFileEvents() {
  timespec timeout_spec = PollTimeout();
  const auto& ready_events = event_poller_->Poll(&timeout_spec);
  processing_file_events_ = true;
  for (const auto& ready_event : ready_events) {
    const int fd = ready_event.fd;
    const int mask = ready_event.mask;
    if (fd < 0 || static_cast<size_t>(fd) >= file_events_.size()) {
      continue;
    }
    const FileEvent* file_event = file_events_[fd].get();
    if (file_event == nullptr) {
      continue;
    }
    int inverted = file_event->Mask() & EventFlag::kBarrier;
    bool fired = false;
    // Normal order is read-before-write; kBarrier lets a pending write flush
    // before another read queues more output on the same descriptor.
    if ((inverted == 0) &&
        ((mask & file_event->Mask() & EventFlag::kReadable) != 0) &&
        file_event->HasReadCallback()) {
      if (file_event->CallReadCallback(this, fd, mask) ==
          CallbackStatus::kError) {
        continue;
      }
      fired = true;
      if (file_events_[fd].get() != file_event) {
        continue;
      }
    }
    if (((mask & file_event->Mask() & EventFlag::kWritable) != 0) &&
        file_event->HasWriteCallback() &&
        (!fired || file_event->HasSeparateCallbacks())) {
      if (file_event->CallWriteCallback(this, fd, mask) ==
          CallbackStatus::kError) {
        continue;
      }
      fired = true;
      if (file_events_[fd].get() != file_event) {
        continue;
      }
    }
    if ((inverted != 0) &&
        ((mask & file_event->Mask() & EventFlag::kReadable) != 0) &&
        file_event->HasReadCallback() &&
        (!fired || file_event->HasSeparateCallbacks())) {
      file_event->CallReadCallback(this, fd, mask);
    }
  }
  processing_file_events_ = false;
  retired_file_events_.clear();
}

void Loop::ProcessDeferredCallbacks() {
  running_deferred_callbacks_.clear();
  running_deferred_callbacks_.swap(deferred_callbacks_);
  for (auto& callback : running_deferred_callbacks_) {
    callback();
  }
  running_deferred_callbacks_.clear();
}

timespec Loop::PollTimeout() const {
  int64_t timeout_ms = kDefaultPollTimeoutMs;
  if (!deferred_callbacks_.empty()) {
    timeout_ms = 0;
  } else {
    const int64_t now = utils::MonotonicMilliseconds();
    for (const auto& event : time_events_) {
      if (event->Deleted() || event->When() <= now) {
        timeout_ms = 0;
        break;
      }
      timeout_ms = std::min(timeout_ms, event->When() - now);
    }
  }

  timespec timeout{};
  timeout.tv_sec = static_cast<time_t>(timeout_ms / kMillisecondsPerSecond);
  timeout.tv_nsec = static_cast<long>((timeout_ms % kMillisecondsPerSecond) *
                                      kNanosecondsPerMillisecond);
  return timeout;
}

void Loop::ProcessTimeEvents() {
  const int64_t now = utils::MonotonicMilliseconds();
  for (auto it = time_events_.begin(); it != time_events_.end();) {
    TimeEvent* const time_event = it->get();
    if (time_event->Deleted()) {
      if (time_event->HasFinalizeCallback()) {
        time_event->CallFinalizeCallback();
      }
      it = time_events_.erase(it);
      continue;
    }
    if (time_event->When() <= now) {
      const int64_t delay_ms = time_event->CallTimeCallback();
      if (delay_ms < 0) {
        // Defer deletion so unlinking and finalization happen in one path.
        time_event->MarkDeleted();
      } else {
        time_event->SetWhen(utils::MonotonicMilliseconds() + delay_ms);
      }
    }
    ++it;
  }
}
}  // namespace redis_simple::event_loop
