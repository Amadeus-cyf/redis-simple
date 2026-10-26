#ifndef REDIS_SIMPLE_EVENT_LOOP_TIME_EVENT_H_
#define REDIS_SIMPLE_EVENT_LOOP_TIME_EVENT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace redis_simple::event_loop {
class TimeEvent {
 public:
  // Returns the delay in milliseconds until the next run, or kNoMore to
  // delete the event.
  using TimeCallback = std::function<int64_t(int64_t id)>;
  using FinalizeCallback = std::function<void()>;

  static constexpr int64_t kNoMore = -1;

  static std::unique_ptr<TimeEvent> Create(TimeCallback time_callback,
                                           FinalizeCallback finalize_callback) {
    if (!time_callback) {
      return nullptr;
    }
    static int64_t next_id = 0;
    return std::unique_ptr<TimeEvent>(new TimeEvent(
        next_id++, std::move(time_callback), std::move(finalize_callback)));
  }

  int64_t Id() const { return id_; }
  // Absolute run time in utils::MonotonicMilliseconds().
  int64_t When() const { return when_; }
  void SetWhen(int64_t when) { when_ = when; }
  bool Deleted() const { return deleted_; }
  void MarkDeleted() { deleted_ = true; }
  bool HasFinalizeCallback() const {
    return static_cast<bool>(finalize_callback_);
  }
  int64_t CallTimeCallback() const { return time_callback_(id_); }
  void CallFinalizeCallback() const { finalize_callback_(); }

 private:
  TimeEvent(int64_t id, TimeCallback time_callback,
            FinalizeCallback finalize_callback)
      : id_(id),
        time_callback_(std::move(time_callback)),
        finalize_callback_(std::move(finalize_callback)) {}

  int64_t id_;
  int64_t when_{0};
  bool deleted_{false};
  TimeCallback time_callback_;
  FinalizeCallback finalize_callback_;
};
}  // namespace redis_simple::event_loop

#endif  // REDIS_SIMPLE_EVENT_LOOP_TIME_EVENT_H_
