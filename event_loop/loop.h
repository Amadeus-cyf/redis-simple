#ifndef REDIS_SIMPLE_EVENT_LOOP_LOOP_H_
#define REDIS_SIMPLE_EVENT_LOOP_LOOP_H_

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "event_loop/event_poller.h"
#include "event_loop/file_event.h"
#include "event_loop/time_event.h"

namespace redis_simple::event_loop {
enum class EventFlag {
  kReadable = 1,
  kWritable = 1 << 1,
  // With kWritable: when a descriptor is both readable and writable, run its
  // write callback before its read callback instead of after it.
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
// C++20 derives the reversed and != forms from this operator.
constexpr bool operator==(int value, EventFlag flag) {
  return value == ToInt(flag);
}

enum class Status {
  kOk = 0,
  kError = -1,
};

// Waits up to timeout_ms (forever if negative) for fd to become readable or
// writable, as `mask` asks. Returns the EventFlag bits that are ready, 0 on
// timeout, or -1 on error. A hang-up or socket error counts as writable.
int WaitForEvent(int fd, int mask, int64_t timeout_ms);
inline int WaitForEvent(int fd, EventFlag mask, int64_t timeout_ms) {
  return WaitForEvent(fd, ToInt(mask), timeout_ms);
}

// A single-threaded event loop over kqueue or epoll: descriptor callbacks,
// timers, and deferred callbacks all run on the thread that calls Run().
class Loop {
 public:
  // Returns nullptr if the poller cannot be created.
  static std::unique_ptr<Loop> Create();
  void Run();
  void Stop() { stop_requested_ = true; }
  // Registers file_event's callbacks for fd. A descriptor has one event, so
  // this adds to the directions already registered for fd.
  Status CreateFileEvent(int fd, std::unique_ptr<FileEvent> file_event);
  // Stops watching the directions in `mask`. Safe to call from a callback.
  Status DeleteFileEvent(int fd, int mask);
  Status DeleteFileEvent(int fd, EventFlag mask) {
    return DeleteFileEvent(fd, ToInt(mask));
  }
  void CreateTimeEvent(std::unique_ptr<TimeEvent> time_event);
  // Runs `callback` once, after the next pass over the file events. A
  // pending callback keeps the poll before that pass from waiting.
  void Defer(std::function<void()> callback);
  // Sets the callback the loop runs once per iteration, right before it waits
  // for events (Redis' beforeSleep). An empty function removes it. The
  // callback must not replace itself.
  void SetBeforeSleep(std::function<void()> callback) {
    before_sleep_ = std::move(callback);
  }
  // Sets the callback the loop runs right after each poll returns, before
  // the ready events (Redis' afterSleep). An empty function removes it.
  void SetAfterSleep(std::function<void()> callback) {
    after_sleep_ = std::move(callback);
  }
  // While set, polls return at once instead of waiting for events (Redis'
  // aeSetDontWait), for a before-sleep callback that expects more work.
  void SetDontWait(bool dont_wait) { dont_wait_ = dont_wait; }
  // Runs one iteration: the before-sleep callback, a poll, the after-sleep
  // callback and the poll's file events, the deferred callbacks, then the
  // time events that are due.
  void ProcessEvents();
  ~Loop() = default;

 private:
  static constexpr int kMaxReadyEvents = 1024;
  explicit Loop(std::unique_ptr<EventPoller> event_poller);
  void ProcessFileEvents();
  void ProcessTimeEvents();
  void ProcessDeferredCallbacks();
  timespec PollTimeout() const;
  // Indexed by descriptor; null where none is registered.
  std::vector<std::unique_ptr<FileEvent>> file_events_;
  // Events deleted or replaced by a callback, kept alive until the current
  // pass over the ready events ends, since one of them may be running.
  std::vector<std::unique_ptr<FileEvent>> retired_file_events_;
  std::list<std::unique_ptr<TimeEvent>> time_events_;
  std::vector<std::function<void()>> deferred_callbacks_;
  std::vector<std::function<void()>> running_deferred_callbacks_;
  std::function<void()> before_sleep_;
  std::function<void()> after_sleep_;
  std::unique_ptr<EventPoller> event_poller_;
  bool processing_file_events_{false};
  bool stop_requested_{false};
  bool dont_wait_{false};
};
}  // namespace redis_simple::event_loop

#endif  // REDIS_SIMPLE_EVENT_LOOP_LOOP_H_
