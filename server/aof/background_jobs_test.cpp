#include "server/aof/background_jobs.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "server/aof/file_ops.h"

namespace redis_simple::aof {
namespace {
// Real system calls, except that closes and deletions can be held back the
// way a slow disk holds them.
class SlowFileOps final : public FileOps {
 public:
  ssize_t Write(int fd, const void* data, size_t size) override {
    return write(fd, data, size);
  }
  ssize_t WriteVector(int fd, const iovec* blocks, int count) override {
    return writev(fd, blocks, count);
  }
  int Sync(int fd) override {
    {
      const std::scoped_lock lock(mutex_);
      synced_.push_back(fd);
    }
    changed_.notify_all();
    return fsync(fd);
  }
  int Rename(const char* source, const char* destination) override {
    return rename(source, destination);
  }
  int Close(int fd) override {
    WaitWhileHeld();
    return close(fd);
  }
  int Unlink(const char* path) override {
    WaitWhileHeld();
    return unlink(path);
  }

  // Closes and deletions block from now until Release().
  void Hold() {
    const std::scoped_lock lock(mutex_);
    held_ = true;
  }
  void Release() {
    {
      const std::scoped_lock lock(mutex_);
      held_ = false;
    }
    changed_.notify_all();
  }
  // Waits for a close or deletion to block. False if none did in time.
  bool WaitForBlockedJob() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kTimeout, [this] { return blocked_ > 0; });
  }
  // Waits until `count` syncs have run. False if they did not in time.
  bool WaitForSyncs(size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kTimeout,
                             [&] { return synced_.size() >= count; });
  }
  std::vector<int> Synced() {
    const std::scoped_lock lock(mutex_);
    return synced_;
  }
  int Blocked() {
    const std::scoped_lock lock(mutex_);
    return blocked_;
  }

 private:
  static constexpr std::chrono::seconds kTimeout{10};

  void WaitWhileHeld() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!held_) {
      return;
    }
    ++blocked_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return !held_; });
    --blocked_;
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  bool held_{false};
  int blocked_{0};
  std::vector<int> synced_;
};

class BackgroundJobsTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string name =
        (std::filesystem::temp_directory_path() / "background-jobs-XXXXXX")
            .string();
    ASSERT_NE(mkdtemp(name.data()), nullptr);
    directory_ = name;
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  [[nodiscard]] std::string Path(const std::string& name) const {
    return (directory_ / name).string();
  }
  // Creates the file and returns a descriptor open on it.
  [[nodiscard]] int Open(const std::string& name) const {
    return OpenFile(Path(name), O_WRONLY | O_CREAT, 0644);
  }
  static bool IsOpen(int fd) { return fcntl(fd, F_GETFD) != -1; }

  SlowFileOps* FileOps() { return &file_ops_; }

 private:
  SlowFileOps file_ops_;
  std::filesystem::path directory_;
};

TEST_F(BackgroundJobsTest, SyncsInOrderAndClosesWhenAsked) {
  BackgroundJobs jobs(FileOps());
  ASSERT_TRUE(jobs.Start());
  const int kept = Open("kept");
  const int closed = Open("closed");
  ASSERT_GE(kept, 0);
  ASSERT_GE(closed, 0);

  jobs.QueueFsync(kept, false);
  jobs.QueueFsync(closed, true);
  jobs.QueueFsync(kept, false);
  jobs.WaitUntilIdle();

  EXPECT_EQ(FileOps()->Synced(), (std::vector<int>{kept, closed, kept}));
  EXPECT_EQ(jobs.PendingFsyncs(), 0U);
  EXPECT_FALSE(jobs.FsyncFailed());
  EXPECT_TRUE(IsOpen(kept));
  EXPECT_FALSE(IsOpen(closed));
  close(kept);
}

TEST_F(BackgroundJobsTest, SlowCloseDoesNotDelayALaterSync) {
  BackgroundJobs jobs(FileOps());
  ASSERT_TRUE(jobs.Start());
  const int old_file = Open("old");
  const int new_file = Open("new");
  ASSERT_GE(old_file, 0);
  ASSERT_GE(new_file, 0);

  // What switching to a new AOF file queues: sync and close the old one,
  // then the every-second sync of the new one.
  FileOps()->Hold();
  jobs.QueueFsync(old_file, true);
  ASSERT_TRUE(FileOps()->WaitForBlockedJob());
  const auto queued = std::chrono::steady_clock::now();
  jobs.QueueFsync(new_file, false);
  ASSERT_TRUE(FileOps()->WaitForSyncs(2));
  while (jobs.PendingFsyncs() != 0) {
    std::this_thread::yield();
  }
  const auto waited = std::chrono::steady_clock::now() - queued;
  RecordProperty(
      "sync_wait_microseconds",
      std::to_string(
          std::chrono::duration_cast<std::chrono::microseconds>(waited)
              .count()));

  // The close is still stuck, and the sync finished without it.
  EXPECT_EQ(FileOps()->Blocked(), 1);
  EXPECT_TRUE(IsOpen(old_file));
  EXPECT_EQ(FileOps()->Synced(), (std::vector<int>{old_file, new_file}));

  FileOps()->Release();
  jobs.WaitUntilIdle();
  EXPECT_FALSE(IsOpen(old_file));
  close(new_file);
}

TEST_F(BackgroundJobsTest, SlowDeletionDoesNotDelayASync) {
  BackgroundJobs jobs(FileOps());
  ASSERT_TRUE(jobs.Start());
  const int file = Open("file");
  ASSERT_GE(file, 0);
  std::ofstream(Path("history")) << "old";

  FileOps()->Hold();
  jobs.QueueDeletion(Path("history"));
  ASSERT_TRUE(FileOps()->WaitForBlockedJob());
  jobs.QueueFsync(file, false);
  ASSERT_TRUE(FileOps()->WaitForSyncs(1));
  while (jobs.PendingFsyncs() != 0) {
    std::this_thread::yield();
  }
  EXPECT_EQ(FileOps()->Blocked(), 1);
  EXPECT_TRUE(std::filesystem::exists(Path("history")));

  FileOps()->Release();
  jobs.WaitUntilIdle();
  EXPECT_FALSE(std::filesystem::exists(Path("history")));
  close(file);
}

TEST_F(BackgroundJobsTest, ClosesEveryFileHandedOverWhileOneCloseIsStuck) {
  BackgroundJobs jobs(FileOps());
  ASSERT_TRUE(jobs.Start());
  constexpr int kFiles = 64;
  std::vector<int> files;
  for (int index = 0; index < kFiles; ++index) {
    files.push_back(Open("file-" + std::to_string(index)));
    ASSERT_GE(files.back(), 0);
  }

  FileOps()->Hold();
  for (const int fd : files) {
    jobs.QueueFsync(fd, true);
  }
  ASSERT_TRUE(FileOps()->WaitForSyncs(kFiles));
  FileOps()->Release();
  jobs.WaitUntilIdle();

  for (const int fd : files) {
    EXPECT_FALSE(IsOpen(fd));
  }
}

TEST_F(BackgroundJobsTest, DeletesFilesAndLongPathsAtOnce) {
  BackgroundJobs jobs(FileOps());
  ASSERT_TRUE(jobs.Start());
  std::ofstream(Path("first")) << "1";
  std::ofstream(Path("second")) << "2";
  jobs.QueueDeletion(Path("first"));
  jobs.QueueDeletion(Path("second"));
  // Too long for the thread's buffer, so it is tried, and fails, right here.
  jobs.QueueDeletion(std::string(5000, 'x'));
  jobs.WaitUntilIdle();
  EXPECT_FALSE(std::filesystem::exists(Path("first")));
  EXPECT_FALSE(std::filesystem::exists(Path("second")));
}

TEST_F(BackgroundJobsTest, StopRunsTheJobsWhenTheThreadsNeverStarted) {
  BackgroundJobs jobs(FileOps());
  const int kept = Open("kept");
  const int closed = Open("closed");
  ASSERT_GE(kept, 0);
  ASSERT_GE(closed, 0);
  std::ofstream(Path("history")) << "old";

  jobs.QueueFsync(kept, false);
  jobs.QueueFsync(closed, true);
  jobs.QueueDeletion(Path("history"));
  EXPECT_EQ(jobs.PendingFsyncs(), 1U);
  jobs.Stop();

  EXPECT_EQ(FileOps()->Synced(), (std::vector<int>{kept, closed}));
  EXPECT_EQ(jobs.PendingFsyncs(), 0U);
  EXPECT_TRUE(IsOpen(kept));
  EXPECT_FALSE(IsOpen(closed));
  EXPECT_FALSE(std::filesystem::exists(Path("history")));
  close(kept);
}

TEST_F(BackgroundJobsTest, StopFinishesWhatIsQueued) {
  const int closed = Open("closed");
  ASSERT_GE(closed, 0);
  std::ofstream(Path("history")) << "old";
  {
    BackgroundJobs jobs(FileOps());
    ASSERT_TRUE(jobs.Start());
    jobs.QueueFsync(closed, true);
    jobs.QueueDeletion(Path("history"));
  }
  EXPECT_FALSE(IsOpen(closed));
  EXPECT_FALSE(std::filesystem::exists(Path("history")));
}
}  // namespace
}  // namespace redis_simple::aof
