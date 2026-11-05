#include "server/io_threads.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace redis_simple {
TEST(IoThreadsTest, RunsEveryIndexExactlyOnce) {
  for (size_t thread_count = 1; thread_count <= 6; ++thread_count) {
    const auto pool = IoThreads::Create(thread_count);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(pool->ThreadCount(), thread_count);
    for (const size_t count : {size_t{0}, size_t{1}, size_t{5}, size_t{11},
                               size_t{12}, size_t{13}, size_t{1000}}) {
      std::vector<std::atomic<int>> calls(count);
      auto job = [&calls](size_t index) {
        calls[index].fetch_add(1, std::memory_order_relaxed);
      };
      pool->Run(count, job);
      for (size_t index = 0; index < count; ++index) {
        EXPECT_EQ(calls[index].load(), 1)
            << "threads " << thread_count << " count " << count << " index "
            << index;
      }
    }
  }
}

TEST(IoThreadsTest, SmallBatchesStayOnTheCallingThread) {
  const auto pool = IoThreads::Create(4);
  ASSERT_NE(pool, nullptr);
  EXPECT_FALSE(pool->Active());

  std::mutex mutex;
  std::set<std::thread::id> threads;
  auto job = [&](size_t /*index*/) {
    const std::lock_guard<std::mutex> lock(mutex);
    threads.insert(std::this_thread::get_id());
  };
  // Below kMinJobsPerThread jobs per thread.
  EXPECT_FALSE(pool->Run(IoThreads::kMinJobsPerThread * 4 - 1, job));
  EXPECT_FALSE(pool->Active());
  EXPECT_EQ(threads, std::set<std::thread::id>{std::this_thread::get_id()});

  // A large batch wakes the workers and spreads over every thread.
  threads.clear();
  EXPECT_TRUE(pool->Run(4000, job));
  EXPECT_TRUE(pool->Active());
  EXPECT_EQ(threads.size(), 4);
  EXPECT_TRUE(threads.contains(std::this_thread::get_id()));

  // The next small batch parks them again.
  threads.clear();
  pool->Run(3, job);
  EXPECT_FALSE(pool->Active());
  EXPECT_EQ(threads, std::set<std::thread::id>{std::this_thread::get_id()});
}

TEST(IoThreadsTest, ResultsAreVisibleWhenRunReturns) {
  // One thread per core keeps the waits short on small machines.
  const size_t thread_count = std::clamp<size_t>(
      std::thread::hardware_concurrency(), size_t{2}, size_t{4});
  const auto pool = IoThreads::Create(thread_count);
  ASSERT_NE(pool, nullptr);
  // Plain writes from the workers must be visible to the caller once Run
  // returns, which is how the server reads each client's I/O result.
  std::vector<size_t> results(512);
  for (size_t round = 0; round < 500; ++round) {
    auto job = [&results, round](size_t index) {
      results[index] = (round * 1000) + index;
    };
    // Alternate large and small batches to exercise parking.
    const size_t count = round % 3 == 0 ? 5 : results.size();
    pool->Run(count, job);
    for (size_t index = 0; index < count; ++index) {
      ASSERT_EQ(results[index], (round * 1000) + index) << round;
    }
  }
}

TEST(IoThreadsTest, DestroysWhetherActiveOrParked) {
  for (const bool wake : {false, true}) {
    auto pool = IoThreads::Create(3);
    ASSERT_NE(pool, nullptr);
    std::atomic<size_t> calls{0};
    auto job = [&calls](size_t /*index*/) {
      calls.fetch_add(1, std::memory_order_relaxed);
    };
    pool->Run(wake ? 100 : 1, job);
    EXPECT_EQ(pool->Active(), wake);
    pool.reset();
    EXPECT_EQ(calls.load(), wake ? 100 : 1);
  }
}

TEST(IoThreadsTest, RejectsZeroThreads) {
  EXPECT_EQ(IoThreads::Create(0), nullptr);
}
}  // namespace redis_simple
