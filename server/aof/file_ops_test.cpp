#include "server/aof/file_ops.h"

#include <gtest/gtest.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "server/aof/snapshot.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/rdb/rdb_writer.h"

namespace redis_simple::aof {
namespace {
constexpr size_t kInterval = IncrementalSync::kIntervalBytes;

// Real writes, and a record of every sync and page cache call in order, so
// tests can check the sequence on any platform. With `range_sync` it claims
// Linux' range sync without calling it.
class RecordingFileOps final : public FileOps {
 public:
  explicit RecordingFileOps(bool range_sync) : range_sync_(range_sync) {}

  ssize_t Write(int fd, const void* data, size_t size) override {
    return write(fd, data, size);
  }
  ssize_t WriteVector(int fd, const iovec* blocks, int count) override {
    return writev(fd, blocks, count);
  }
  int Sync(int /*fd*/) override { return Record("sync"); }
  int Rename(const char* source, const char* destination) override {
    return std::rename(source, destination);
  }
  [[nodiscard]] bool HasRangeSync() const override { return range_sync_; }
  int StartWriteback(int /*fd*/, off_t offset, off_t size) override {
    return Record("start " + Range(offset, size));
  }
  int WaitForWriteback(int /*fd*/, off_t offset, off_t size) override {
    return Record("wait " + Range(offset, size));
  }
  int DropPageCache(int /*fd*/) override { return Record("drop"); }

  // Makes the call with this number, counting from 1, fail with EIO.
  void FailCall(size_t number) { failing_call_ = number; }
  [[nodiscard]] const std::vector<std::string>& Calls() const { return calls_; }

 private:
  // A range as "<first interval>+<intervals>", so expectations stay short.
  static std::string Range(off_t offset, off_t size) {
    const auto interval = static_cast<double>(kInterval);
    return std::to_string(static_cast<double>(offset) / interval) + "+" +
           std::to_string(static_cast<double>(size) / interval);
  }

  int Record(std::string call) {
    calls_.push_back(std::move(call));
    if (calls_.size() == failing_call_) {
      errno = EIO;
      return -1;
    }
    return 0;
  }

  bool range_sync_;
  size_t failing_call_{};
  std::vector<std::string> calls_;
};

using Calls = std::vector<std::string>;

TEST(IncrementalSyncTest, RangeSyncStartsEachIntervalAndWaitsForTheOneBefore) {
  RecordingFileOps file_ops(/*range_sync=*/true);
  IncrementalSync sync(&file_ops, -1, /*keep_page_cache=*/false);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(sync.Written(kInterval));
  }
  EXPECT_EQ(file_ops.Calls(), (Calls{
                                  "start 0.000000+1.000000",
                                  "drop",
                                  "start 1.000000+1.000000",
                                  "wait 0.000000+1.000000",
                                  "drop",
                                  "start 2.000000+1.000000",
                                  "wait 1.000000+1.000000",
                                  "drop",
                              }));
}

TEST(IncrementalSyncTest, IntervalsCoverEveryByteWhateverTheWriteSizes) {
  RecordingFileOps file_ops(/*range_sync=*/true);
  IncrementalSync sync(&file_ops, -1, /*keep_page_cache=*/true);
  // Nothing happens before an interval's worth is written.
  ASSERT_TRUE(sync.Written(kInterval / 2));
  EXPECT_TRUE(file_ops.Calls().empty());
  // A write may overshoot the interval; the next range starts where this
  // one ended.
  ASSERT_TRUE(sync.Written(kInterval));
  ASSERT_TRUE(sync.Written(kInterval / 2));
  ASSERT_TRUE(sync.Written(kInterval * 2));
  EXPECT_EQ(file_ops.Calls(), (Calls{
                                  "start 0.000000+1.500000",
                                  "start 1.500000+2.500000",
                                  "wait 0.000000+1.500000",
                              }));
}

TEST(IncrementalSyncTest, SyncsTheWholeFileWithoutRangeSync) {
  RecordingFileOps file_ops(/*range_sync=*/false);
  IncrementalSync sync(&file_ops, -1, /*keep_page_cache=*/false);
  ASSERT_TRUE(sync.Written(kInterval - 1));
  EXPECT_TRUE(file_ops.Calls().empty());
  ASSERT_TRUE(sync.Written(1));
  ASSERT_TRUE(sync.Written(kInterval));
  EXPECT_EQ(file_ops.Calls(), (Calls{"sync", "drop", "sync", "drop"}));
}

TEST(IncrementalSyncTest, KeepsThePageCacheWhenAsked) {
  RecordingFileOps file_ops(/*range_sync=*/false);
  IncrementalSync sync(&file_ops, -1, /*keep_page_cache=*/true);
  ASSERT_TRUE(sync.Written(kInterval));
  EXPECT_EQ(file_ops.Calls(), Calls{"sync"});
}

TEST(IncrementalSyncTest, ReportsAFailedSyncButNotAFailedPageCacheDrop) {
  // Calls of two intervals: start, drop, start, wait, drop.
  for (const size_t failing_call : {1, 3, 4}) {
    RecordingFileOps file_ops(/*range_sync=*/true);
    file_ops.FailCall(failing_call);
    IncrementalSync sync(&file_ops, -1, /*keep_page_cache=*/false);
    const bool first = sync.Written(kInterval);
    EXPECT_EQ(first, failing_call != 1);
    if (first) {
      EXPECT_FALSE(sync.Written(kInterval)) << failing_call;
      EXPECT_EQ(errno, EIO);
    }
  }
  RecordingFileOps file_ops(/*range_sync=*/true);
  file_ops.FailCall(2);
  IncrementalSync sync(&file_ops, -1, /*keep_page_cache=*/false);
  EXPECT_TRUE(sync.Written(kInterval));
  EXPECT_TRUE(sync.Written(kInterval));

  RecordingFileOps plain(/*range_sync=*/false);
  plain.FailCall(1);
  IncrementalSync plain_sync(&plain, -1, /*keep_page_cache=*/false);
  EXPECT_FALSE(plain_sync.Written(kInterval));
}

TEST(IncrementalSyncTest, FileOperationsWithoutRangeSyncRefuseItsCalls) {
  // What file operations that only inject failures inherit.
  class Plain final : public FileOps {
   public:
    ssize_t Write(int /*fd*/, const void* /*data*/, size_t /*size*/) override {
      return 0;
    }
    ssize_t WriteVector(int /*fd*/, const iovec* /*blocks*/,
                        int /*count*/) override {
      return 0;
    }
    int Sync(int /*fd*/) override { return 0; }
    int Rename(const char* /*source*/, const char* /*destination*/) override {
      return 0;
    }
  };
  Plain plain;
  EXPECT_FALSE(plain.HasRangeSync());
  EXPECT_EQ(plain.StartWriteback(-1, 0, 1), -1);
  EXPECT_EQ(errno, ENOTSUP);
  EXPECT_EQ(plain.WaitForWriteback(-1, 0, 1), -1);
  EXPECT_EQ(errno, ENOTSUP);
  EXPECT_EQ(plain.DropPageCache(-1), 0);
}

// The snapshot writers, in a directory of their own.
class SnapshotSyncTest : public testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("redis_simple_snapshot_sync_" + std::to_string(getpid()));
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
    path_ = (directory_ / "snapshot").string();
    // Five 3 MiB values. The RDB writer hands each over in one write, so
    // its ranges are 6 MiB; either format syncs at least two of them.
    db_ = db::RedisDb::Create();
    for (const char* key : {"a", "b", "c", "d", "e"}) {
      db_->SetKey(key,
                  db::RedisObject::CreateWithString(
                      std::string(size_t{3} * 1024 * 1024, 'x')),
                  0);
    }
  }

  void TearDown() override { std::filesystem::remove_all(directory_); }

  static size_t Count(const Calls& calls, const std::string& call) {
    return static_cast<size_t>(std::ranges::count(calls, call));
  }

  // Every range started, each but the last waited for, and the plain sync
  // still the last one before the file is renamed.
  static void ExpectRangeSyncs(const Calls& calls, bool drops_page_cache) {
    size_t starts = 0;
    size_t waits = 0;
    for (const std::string& call : calls) {
      starts += call.starts_with("start ") ? 1 : 0;
      waits += call.starts_with("wait ") ? 1 : 0;
    }
    EXPECT_GE(starts, 2);
    EXPECT_EQ(waits, starts - 1);
    EXPECT_EQ(Count(calls, "sync"), 1);
    if (drops_page_cache) {
      EXPECT_EQ(Count(calls, "drop"), starts + 1);
      ASSERT_GE(calls.size(), 2);
      EXPECT_EQ(calls[calls.size() - 2], "sync");
      EXPECT_EQ(calls.back(), "drop");
    } else {
      EXPECT_EQ(Count(calls, "drop"), 0);
      ASSERT_FALSE(calls.empty());
      EXPECT_EQ(calls.back(), "sync");
    }
  }

  [[nodiscard]] db::RedisDb* Db() const { return db_.get(); }
  [[nodiscard]] const std::string& Path() const { return path_; }

 private:
  std::filesystem::path directory_;
  std::string path_;
  std::unique_ptr<db::RedisDb> db_;
};

TEST_F(SnapshotSyncTest, RdbWriterSyncsRangesThenTheFileAndDropsItsPages) {
  RecordingFileOps file_ops(/*range_sync=*/true);
  ASSERT_EQ(rdb::WriteRdbFile(Db(), Path(), {.compression = false}, &file_ops),
            rdb::WriteStatus::kWritten);
  ExpectRangeSyncs(file_ops.Calls(), /*drops_page_cache=*/true);
}

TEST_F(SnapshotSyncTest, RdbWriterKeepsThePageCacheWhenAsked) {
  RecordingFileOps file_ops(/*range_sync=*/true);
  ASSERT_EQ(rdb::WriteRdbFile(Db(), Path(),
                              {.compression = false, .keep_page_cache = true},
                              &file_ops),
            rdb::WriteStatus::kWritten);
  ExpectRangeSyncs(file_ops.Calls(), /*drops_page_cache=*/false);
}

TEST_F(SnapshotSyncTest, RdbWriterOnlySyncsAtTheEndWithoutIncrementalFsync) {
  RecordingFileOps file_ops(/*range_sync=*/true);
  ASSERT_EQ(rdb::WriteRdbFile(
                Db(), Path(),
                {.compression = false, .incremental_fsync = false}, &file_ops),
            rdb::WriteStatus::kWritten);
  EXPECT_EQ(file_ops.Calls(), (Calls{"sync", "drop"}));
}

TEST_F(SnapshotSyncTest, RdbWriterReportsAFailedRangeSync) {
  RecordingFileOps file_ops(/*range_sync=*/true);
  file_ops.FailCall(1);
  EXPECT_EQ(rdb::WriteRdbFile(Db(), Path(), {.compression = false}, &file_ops),
            rdb::WriteStatus::kSyncFailed);
  EXPECT_FALSE(PathExists(Path()));
}

TEST_F(SnapshotSyncTest, CommandSnapshotSyncsRangesThenTheFileAndDropsPages) {
  RecordingFileOps file_ops(/*range_sync=*/true);
  ASSERT_EQ(WriteSnapshotFile(Db(), Path(), {}, &file_ops),
            SnapshotStatus::kWritten);
  ExpectRangeSyncs(file_ops.Calls(), /*drops_page_cache=*/true);
}

TEST_F(SnapshotSyncTest, CommandSnapshotReportsAFailedRangeSync) {
  RecordingFileOps file_ops(/*range_sync=*/true);
  file_ops.FailCall(1);
  EXPECT_EQ(WriteSnapshotFile(Db(), Path(), {}, &file_ops),
            SnapshotStatus::kSyncFailed);
  EXPECT_FALSE(PathExists(Path()));
}

TEST_F(SnapshotSyncTest, SystemFileOperationsWriteALoadableSnapshot) {
  // The real calls of this platform: range sync on Linux, fsync elsewhere.
  ASSERT_EQ(rdb::WriteRdbFile(Db(), Path(), {.compression = false},
                              DefaultFileOps().get()),
            rdb::WriteStatus::kWritten);
  EXPECT_GT(PathSize(Path()).value_or(0), size_t{9} * 1024 * 1024);
}
}  // namespace
}  // namespace redis_simple::aof
