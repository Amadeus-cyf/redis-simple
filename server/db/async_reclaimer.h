#ifndef REDIS_SIMPLE_SERVER_DB_ASYNC_RECLAIMER_H_
#define REDIS_SIMPLE_SERVER_DB_ASYNC_RECLAIMER_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

#include "server/db/redis_obj.h"

namespace redis_simple::db {
class AsyncReclaimer {
 public:
  AsyncReclaimer();
  AsyncReclaimer(const AsyncReclaimer&) = delete;
  AsyncReclaimer& operator=(const AsyncReclaimer&) = delete;
  bool Reclaim(RedisObjectPtr object);
  void WaitUntilIdle();
  size_t PendingCount() const;
  ~AsyncReclaimer();

 private:
  void Run();

  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::deque<RedisObjectPtr> pending_;
  size_t reclaiming_{};
  bool stopping_{};
  // Declared last so every field the worker reads is initialized before the
  // thread starts.
  std::thread worker_;
};
}  // namespace redis_simple::db

#endif  // REDIS_SIMPLE_SERVER_DB_ASYNC_RECLAIMER_H_
