#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "event_loop/event_poller.h"
#include "event_loop/file_event.h"
#include "event_loop/time_event.h"

namespace redis_simple::event_loop {
enum class EventFlag {
  kReadable = 1,
  kWritable = 1 << 1,
  kBarrier = 1 << 2,
};

constexpr int ToInt(EventFlag flag) { return static_cast<int>(flag); }
inline int& operator|=(int& mask, EventFlag flag) {
  mask |= ToInt(flag);
  return mask;
}
inline int& operator&=(int& mask, EventFlag flag) {
  mask &= ToInt(flag);
  return mask;
}
constexpr int operator&(int mask, EventFlag flag) { return mask & ToInt(flag); }
constexpr int operator&(EventFlag flag, int mask) { return ToInt(flag) & mask; }
constexpr int operator~(EventFlag flag) { return ~ToInt(flag); }
constexpr bool operator==(int value, EventFlag flag) {
  return value == ToInt(flag);
}
constexpr bool operator!=(int value, EventFlag flag) {
  return value != ToInt(flag);
}
constexpr bool operator==(EventFlag flag, int value) { return value == flag; }
constexpr bool operator!=(EventFlag flag, int value) { return value != flag; }

enum class Status {
  kOk = 0,
  kError = -1,
};

// Return the readiness mask for fd, or 0 on timeout.
int WaitForEvent(int fd, int mask, int64_t timeout_ms);
inline int WaitForEvent(int fd, EventFlag mask, int64_t timeout_ms) {
  return WaitForEvent(fd, ToInt(mask), timeout_ms);
}

class Loop {
 public:
  static std::unique_ptr<Loop> Create();
  void Run();
  void Stop() { stop_requested_ = true; }
  Status CreateFileEvent(int fd, std::unique_ptr<FileEvent> file_event);
  Status DeleteFileEvent(int fd, int mask);
  Status DeleteFileEvent(int fd, EventFlag mask) {
    return DeleteFileEvent(fd, ToInt(mask));
  }
  void CreateTimeEvent(std::unique_ptr<TimeEvent> time_event);
  void Defer(std::function<void()> callback);
  void ProcessEvents();
  ~Loop() = default;

 private:
  static constexpr int kMaxReadyEvents = 1024;
  explicit Loop(std::unique_ptr<EventPoller> event_poller);
  void ProcessFileEvents();
  void ProcessTimeEvents();
  void ProcessDeferredCallbacks();
  timespec PollTimeout() const;
  std::vector<std::unique_ptr<FileEvent>> file_events_;
  std::vector<std::unique_ptr<FileEvent>> retired_file_events_;
  std::list<std::unique_ptr<TimeEvent>> time_events_;
  std::vector<std::function<void()>> deferred_callbacks_;
  std::vector<std::function<void()>> running_deferred_callbacks_;
  std::unique_ptr<EventPoller> event_poller_;
  bool processing_file_events_{false};
  bool stop_requested_{false};
};
}  // namespace redis_simple::event_loop
