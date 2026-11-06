#include "event_loop/loop.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace redis_simple::event_loop {
namespace {
class ScopedFd {
 public:
  explicit ScopedFd(int fd = -1) : fd_(fd) {}
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~ScopedFd() { Close(); }

  [[nodiscard]] int Get() const { return fd_; }

 private:
  void Close() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }
  int fd_;
};

std::array<ScopedFd, 2> CreateSocketPair() {
  std::array<int, 2> fds{-1, -1};
  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0)
      << std::strerror(errno);
  return {ScopedFd(fds[0]), ScopedFd(fds[1])};
}
}  // namespace

TEST(LoopTest, FileReadAndWriteCallbacksFire) {
  auto loop = Loop::Create();
  ASSERT_NE(loop, nullptr);
  auto sockets = CreateSocketPair();
  ASSERT_GE(sockets[0].Get(), 0);

  int read_count = 0;
  int write_count = 0;
  auto read_callback = [&read_count](Loop*, int fd, int mask) {
    EXPECT_NE(mask & EventFlag::kReadable, 0);
    char byte = '\0';
    EXPECT_EQ(read(fd, &byte, 1), 1);
    EXPECT_EQ(byte, 'x');
    ++read_count;
    return CallbackStatus::kOk;
  };
  auto write_callback = [&write_count](Loop*, int, int mask) {
    EXPECT_NE(mask & EventFlag::kWritable, 0);
    ++write_count;
    return CallbackStatus::kOk;
  };

  ASSERT_EQ(loop->CreateFileEvent(
                sockets[0].Get(),
                FileEvent::Create(
                    FileEvent::Callback(read_callback),
                    FileEvent::Callback(write_callback),
                    ToInt(EventFlag::kReadable) | ToInt(EventFlag::kWritable))),
            Status::kOk);
  ASSERT_EQ(write(sockets[1].Get(), "x", 1), 1);

  loop->ProcessEvents();

  EXPECT_EQ(read_count, 1);
  EXPECT_EQ(write_count, 1);
}

TEST(LoopTest, DeleteOneFileEventMaskKeepsTheOtherMask) {
  auto loop = Loop::Create();
  ASSERT_NE(loop, nullptr);
  auto sockets = CreateSocketPair();
  ASSERT_GE(sockets[0].Get(), 0);

  int read_count = 0;
  int write_count = 0;
  auto read_callback = [&read_count](Loop*, int fd, int) {
    char byte = '\0';
    EXPECT_EQ(read(fd, &byte, 1), 1);
    ++read_count;
    return CallbackStatus::kOk;
  };
  auto write_callback = [&write_count](Loop*, int, int) {
    ++write_count;
    return CallbackStatus::kOk;
  };

  ASSERT_EQ(loop->CreateFileEvent(
                sockets[0].Get(),
                FileEvent::Create(FileEvent::Callback(read_callback),
                                  FileEvent::Callback(),
                                  ToInt(EventFlag::kReadable))),
            Status::kOk);
  ASSERT_EQ(loop->CreateFileEvent(
                sockets[0].Get(),
                FileEvent::Create(FileEvent::Callback(),
                                  FileEvent::Callback(write_callback),
                                  ToInt(EventFlag::kWritable))),
            Status::kOk);
  ASSERT_EQ(write(sockets[1].Get(), "a", 1), 1);
  loop->ProcessEvents();
  ASSERT_EQ(read_count, 1);
  ASSERT_EQ(write_count, 1);

  ASSERT_EQ(loop->DeleteFileEvent(sockets[0].Get(), EventFlag::kReadable),
            Status::kOk);
  loop->ProcessEvents();

  EXPECT_EQ(read_count, 1);
  EXPECT_EQ(write_count, 2);
}

TEST(LoopTest, TimeEventRunsAndFinalizes) {
  auto loop = Loop::Create();
  ASSERT_NE(loop, nullptr);

  int run_count = 0;
  int finalize_count = 0;
  loop->CreateTimeEvent(TimeEvent::Create(
      [&run_count](int64_t) {
        ++run_count;
        return TimeEvent::kNoMore;
      },
      [&finalize_count] {
        ++finalize_count;
        return 0;
      }));

  loop->ProcessEvents();
  EXPECT_EQ(run_count, 1);
  EXPECT_EQ(finalize_count, 0);

  loop->ProcessEvents();
  EXPECT_EQ(run_count, 1);
  EXPECT_EQ(finalize_count, 1);
}

TEST(LoopTest, TimeEventRequiresCallback) {
  EXPECT_EQ(TimeEvent::Create(nullptr, nullptr), nullptr);
}

TEST(LoopTest, CallbackCanDeleteItsOwnFileEvent) {
  auto loop = Loop::Create();
  ASSERT_NE(loop, nullptr);
  auto sockets = CreateSocketPair();
  ASSERT_GE(sockets[0].Get(), 0);

  int read_count = 0;
  int write_count = 0;
  const int event_mask =
      ToInt(EventFlag::kReadable) | ToInt(EventFlag::kWritable);
  auto read_callback = [&read_count](Loop* callback_loop, int fd, int) {
    char byte = '\0';
    EXPECT_EQ(read(fd, &byte, 1), 1);
    ++read_count;
    EXPECT_EQ(callback_loop->DeleteFileEvent(fd, event_mask), Status::kOk);
    return CallbackStatus::kOk;
  };
  auto write_callback = [&write_count](Loop*, int, int) {
    ++write_count;
    return CallbackStatus::kOk;
  };
  ASSERT_EQ(
      loop->CreateFileEvent(
          sockets[0].Get(),
          FileEvent::Create(FileEvent::Callback(read_callback),
                            FileEvent::Callback(write_callback), event_mask)),
      Status::kOk);
  ASSERT_EQ(write(sockets[1].Get(), "x", 1), 1);

  loop->ProcessEvents();

  EXPECT_EQ(read_count, 1);
  EXPECT_EQ(write_count, 0);
  EXPECT_EQ(loop->DeleteFileEvent(sockets[0].Get(), event_mask),
            Status::kError);
}

TEST(LoopTest, DeferredCallbacksRunAfterFileCallbacks) {
  auto loop = Loop::Create();
  ASSERT_NE(loop, nullptr);
  auto sockets = CreateSocketPair();
  std::vector<int> order;
  auto read_callback = [&order](Loop* callback_loop, int fd, int) {
    char byte = '\0';
    EXPECT_EQ(read(fd, &byte, 1), 1);
    order.push_back(1);
    callback_loop->Defer([&order] { order.push_back(2); });
    return CallbackStatus::kOk;
  };
  ASSERT_EQ(loop->CreateFileEvent(
                sockets[0].Get(),
                FileEvent::Create(FileEvent::Callback(read_callback), nullptr,
                                  ToInt(EventFlag::kReadable))),
            Status::kOk);
  ASSERT_EQ(write(sockets[1].Get(), "x", 1), 1);

  loop->ProcessEvents();

  EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(LoopTest, CallbackDeferredDuringDispatchRunsNextCycle) {
  auto loop = Loop::Create();
  ASSERT_NE(loop, nullptr);
  std::vector<int> order;
  loop->Defer([&] {
    order.push_back(1);
    loop->Defer([&order] { order.push_back(2); });
  });

  loop->ProcessEvents();
  EXPECT_EQ(order, (std::vector<int>{1}));

  loop->ProcessEvents();
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
}
}  // namespace redis_simple::event_loop
