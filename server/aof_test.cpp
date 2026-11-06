#include "server/aof.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/reply.h"
#include "utils/check.h"
#include "utils/time_utils.h"

namespace redis_simple::aof {
namespace {
class TempFile {
 public:
  TempFile() {
    constexpr std::string_view kTemplate = "/tmp/redis_simple_aof_XXXXXX";
    std::array<char, kTemplate.size() + 1> path{};
    std::ranges::copy(kTemplate, path.begin());
    const int fd = mkstemp(path.data());
    RS_CHECK(fd >= 0);
    close(fd);
    path_ = path.data();
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;
  ~TempFile() { unlink(path_.c_str()); }

  [[nodiscard]] const std::string& Path() const { return path_; }

  [[nodiscard]] bool Append(std::string_view data) const {
    const int fd = open(path_.c_str(), O_WRONLY | O_APPEND);
    if (fd < 0) {
      return false;
    }
    size_t written = 0;
    while (written < data.size()) {
      const ssize_t result =
          write(fd, data.data() + written, data.size() - written);
      if (result > 0) {
        written += static_cast<size_t>(result);
      } else if (result < 0 && errno == EINTR) {
        continue;
      } else {
        close(fd);
        return false;
      }
    }
    return close(fd) == 0;
  }

  [[nodiscard]] off_t Size() const {
    struct stat info{};
    return stat(path_.c_str(), &info) == 0 ? info.st_size : -1;
  }

 private:
  std::string path_;
};

class TestFileOps final : public FileOps {
 public:
  ssize_t Write(int fd, const void* data, size_t size) override {
    std::this_thread::sleep_for(write_delay_);
    if (fail_write_.load()) {
      errno = EIO;
      return -1;
    }
    return write(fd, data, std::min(size, max_write_size_));
  }

  ssize_t WriteVector(int fd, const iovec* blocks, int count) override {
    if (fail_write_vector_.load()) {
      errno = EIO;
      return -1;
    }
    if (count <= 0) {
      return 0;
    }
    const size_t size = std::min(blocks[0].iov_len, max_write_size_);
    return write(fd, blocks[0].iov_base, size);
  }

  int Sync(int fd) override {
    synced_fd_flags_.store(fcntl(fd, F_GETFD));
    const int call = sync_calls_.fetch_add(1) + 1;
    if (call == fail_sync_call_.load()) {
      errno = EIO;
      return -1;
    }
    return fsync(fd);
  }

  int Rename(const char* source, const char* destination) override {
    if (fail_rename_.load()) {
      errno = EIO;
      return -1;
    }
    return std::rename(source, destination);
  }

  void LimitWrites(size_t size) { max_write_size_ = size; }
  // Makes each write take this long, like a busy disk.
  void SlowWrites(std::chrono::milliseconds delay) { write_delay_ = delay; }
  void FailWrites() { fail_write_.store(true); }
  void RestoreWrites() { fail_write_.store(false); }
  void FailRewriteWrites() { fail_write_vector_.store(true); }
  void FailNextSync() { fail_sync_call_.store(sync_calls_.load() + 1); }
  void FailSyncAfterNext() { fail_sync_call_.store(sync_calls_.load() + 2); }
  void FailRenames() { fail_rename_.store(true); }
  [[nodiscard]] int SyncCount() const { return sync_calls_.load(); }
  // The descriptor flags (F_GETFD) of the most recently synced file.
  [[nodiscard]] int SyncedFdFlags() const { return synced_fd_flags_.load(); }

 private:
  size_t max_write_size_{std::numeric_limits<size_t>::max()};
  std::chrono::milliseconds write_delay_{0};
  std::atomic<bool> fail_write_;
  std::atomic<bool> fail_write_vector_;
  std::atomic<bool> fail_rename_;
  std::atomic<int> sync_calls_;
  std::atomic<int> fail_sync_call_{-1};
  std::atomic<int> synced_fd_flags_{-1};
};

std::string Encode(std::string_view command,
                   const std::vector<std::string_view>& args) {
  std::string encoded = reply::FromArrayHeader(args.size() + 1);
  reply::AppendBulkString(command, &encoded);
  for (std::string_view arg : args) {
    reply::AppendBulkString(arg, &encoded);
  }
  return encoded;
}

Options Always(const TempFile& file) {
  return {file.Path(), FsyncPolicy::kAlways};
}

Options Always(const TempFile& file, const std::shared_ptr<FileOps>& file_ops) {
  Options options = Always(file);
  options.file_ops = file_ops;
  return options;
}
}  // namespace

TEST(AofTest, ReplaysCanonicalSetAndExpiration) {
  TempFile file;
  auto source = db::RedisDb::Create();
  const int64_t expire = utils::NowInMilliseconds() + 60'000;
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), expire),
      db::DbStatus::kOk);

  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);
  const std::vector<std::string_view> args = {"key", "value", "PX", "60000"};
  ASSERT_TRUE(writer->Append("SET", args, source.get()));
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  const db::RedisObject* object = restored->LookupKey("key");
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(object->String(), "value");
  EXPECT_EQ(restored->Expiration("key"), expire);
}

TEST(AofTest, ReplaysCanonicalConditionalExpiration) {
  TempFile file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);

  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  constexpr int64_t kExpire = 4'102'444'800'000;
  ASSERT_EQ(
      source->ExpireKeyAt("key", kExpire, db::ExpireCondition::kIfNoExpiry),
      db::DbStatus::kOk);
  ASSERT_TRUE(
      writer->Append("EXPIREAT", {"key", "4102444800", "NX"}, source.get()));
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->Expiration("key"), kExpire);
}

TEST(AofTest, DoesNotResurrectExpiredKeysDuringReplay) {
  TempFile file;
  std::string commands = Encode("SET", {"key", "value"});
  commands.append(Encode("PEXPIREAT", {"key", "1"}));
  commands.append(Encode("APPEND", {"key", "suffix"}));
  ASSERT_TRUE(file.Append(commands));

  auto restored = db::RedisDb::Create();
  auto aof = Aof::Open(Always(file), restored.get());

  ASSERT_NE(aof, nullptr);
  EXPECT_EQ(restored->LookupKey("key"), nullptr);
}

TEST(AofTest, TruncatesIncompleteFinalCommand) {
  TempFile file;
  const std::string complete = Encode("SET", {"key", "value"});
  ASSERT_TRUE(file.Append(complete));
  ASSERT_TRUE(file.Append("*2\r\n$3\r\nDEL\r\n$4\r\npar"));
  ASSERT_GT(file.Size(), static_cast<off_t>(complete.size()));

  auto restored = db::RedisDb::Create();
  auto aof = Aof::Open(Always(file), restored.get());

  ASSERT_NE(aof, nullptr);
  EXPECT_EQ(file.Size(), static_cast<off_t>(complete.size()));
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "value");
}

TEST(AofTest, RejectsCorruptInput) {
  TempFile file;
  ASSERT_TRUE(file.Append("not-resp\r\n"));
  auto database = db::RedisDb::Create();

  EXPECT_EQ(Aof::Open(Always(file), database.get()), nullptr);
}

TEST(AofTest, FlushesThePendingBatchOnShutdown) {
  TempFile file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  Options options{file.Path(), FsyncPolicy::kNo};
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  const std::vector<std::string_view> args = {"key", "value"};
  ASSERT_TRUE(writer->Append("SET", args, source.get()));
  EXPECT_TRUE(writer->AcceptingWrites());
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "value");
}

TEST(AofTest, RewritesAllObjectTypesAndPreservesConcurrentWrites) {
  TempFile file;
  auto source = db::RedisDb::Create();
  const int64_t expire = utils::NowInMilliseconds() + 60'000;
  ASSERT_EQ(source->SetKey("string", db::RedisObject::CreateWithString("old"),
                           expire),
            db::DbStatus::kOk);

  auto set_value = set::Set::Create();
  auto list_value = list::List::Create();
  auto zset_value = zset::ZSet::Create();
  auto hash_value = hash::Hash::Create();
  for (int index = 0; index < 65; ++index) {
    const std::string suffix = std::to_string(index);
    ASSERT_TRUE(set_value->Add("member" + suffix));
    ASSERT_TRUE(list_value->RPush("value" + suffix));
    ASSERT_TRUE(zset_value->InsertOrUpdate("member" + suffix, index));
    ASSERT_TRUE(hash_value->Set("field" + suffix, "value" + suffix));
  }
  ASSERT_EQ(source->SetKey(
                "set", db::RedisObject::CreateWithSet(std::move(set_value)), 0),
            db::DbStatus::kOk);

  ASSERT_EQ(
      source->SetKey("list",
                     db::RedisObject::CreateWithList(std::move(list_value)), 0),
      db::DbStatus::kOk);

  ASSERT_EQ(
      source->SetKey("zset",
                     db::RedisObject::CreateWithZSet(std::move(zset_value)), 0),
      db::DbStatus::kOk);

  ASSERT_EQ(
      source->SetKey("hash",
                     db::RedisObject::CreateWithHash(std::move(hash_value)), 0),
      db::DbStatus::kOk);

  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);

  ASSERT_EQ(source->SetKey(
                "string", db::RedisObject::CreateWithString("updated"), expire),
            db::DbStatus::kOk);
  const std::vector<std::string_view> set_args = {"string", "updated"};
  ASSERT_TRUE(writer->Append("SET", set_args, source.get()));
  writer->WaitForRewrite();
  ASSERT_TRUE(writer->AcceptingWrites());
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("string"), nullptr);
  EXPECT_EQ(restored->LookupKey("string")->String(), "updated");
  EXPECT_EQ(restored->Expiration("string"), expire);
  ASSERT_NE(restored->LookupKey("set"), nullptr);
  EXPECT_EQ(restored->LookupKey("set")->Set()->Size(), 65);
  EXPECT_TRUE(restored->LookupKey("set")->Set()->HasMember("member64"));
  ASSERT_NE(restored->LookupKey("list"), nullptr);
  EXPECT_EQ(restored->LookupKey("list")->List()->Size(), 65);
  EXPECT_EQ(restored->LookupKey("list")->List()->At(64), "value64");
  ASSERT_NE(restored->LookupKey("zset"), nullptr);
  EXPECT_EQ(restored->LookupKey("zset")->ZSet()->Size(), 65);
  EXPECT_EQ(restored->LookupKey("zset")->ZSet()->Score("member64"), 64.0);
  ASSERT_NE(restored->LookupKey("hash"), nullptr);
  EXPECT_EQ(restored->LookupKey("hash")->Hash()->Size(), 65);
  EXPECT_EQ(restored->LookupKey("hash")->Hash()->Get("field64"), "value64");
}

TEST(AofTest, RewriteCompactsCommandHistory) {
  TempFile file;
  auto source = db::RedisDb::Create();
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);

  for (int value = 0; value < 128; ++value) {
    const std::string text = std::to_string(value);
    ASSERT_EQ(source->SetKey("key", db::RedisObject::CreateWithString(text), 0),
              db::DbStatus::kOk);
    const std::vector<std::string_view> args = {"key", text};
    ASSERT_TRUE(writer->Append("SET", args, source.get()));
  }
  ASSERT_TRUE(writer->Flush());
  const off_t original_size = file.Size();
  ASSERT_GT(original_size, 0);

  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();

  EXPECT_GT(original_size, file.Size());
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "127");
}

TEST(AofTest, RewritesLargeValuesAsReplayableChunks) {
  TempFile file;
  auto source = db::RedisDb::Create();
  const std::string value(1024, 'x');
  ASSERT_EQ(
      source->SetKey("large", db::RedisObject::CreateWithString(value), 0),
      db::DbStatus::kOk);

  Options options = Always(file);
  options.limits.max_replay_command_bytes = 256;
  options.limits.snapshot_block_bytes = 96;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  EXPECT_EQ(writer->State().rewrite_status, RewriteStatus::kSucceeded);
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(options, restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("large"), nullptr);
  EXPECT_EQ(restored->LookupKey("large")->String(), value);
}

TEST(AofTest, BoundsCollectionSnapshotCommandsByBytes) {
  TempFile file;
  auto source = db::RedisDb::Create();
  auto values = list::List::Create();
  for (int index = 0; index < 8; ++index) {
    ASSERT_TRUE(values->RPush(std::string(80, static_cast<char>('a' + index))));
  }
  ASSERT_EQ(source->SetKey(
                "list", db::RedisObject::CreateWithList(std::move(values)), 0),
            db::DbStatus::kOk);

  Options options = Always(file);
  options.limits.max_replay_command_bytes = 256;
  options.limits.snapshot_block_bytes = 128;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(options, restored.get());
  ASSERT_NE(reader, nullptr);
  const db::RedisObject* object = restored->LookupKey("list");
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(object->List()->Size(), 8);
  EXPECT_EQ(object->List()->At(7), std::string(80, 'h'));
}

TEST(AofTest, HandlesShortWritesWithoutLosingCommands) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  file_ops->LimitWrites(3);
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);

  auto writer = Aof::Open(Always(file, file_ops), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "value");
}

TEST(AofTest, HandlesShortWritesDuringRewrite) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  file_ops->LimitWrites(3);
  auto source = db::RedisDb::Create();
  Options options{file.Path(), FsyncPolicy::kNo};
  options.file_ops = file_ops;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);

  ASSERT_EQ(
      source->SetKey("first", db::RedisObject::CreateWithString("one"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"first", "one"}, source.get()));
  // The snapshot (in the child) and the buffered mutations (in the parent)
  // are written with vectored writes, three bytes at a time.
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  ASSERT_EQ(
      source->SetKey("second", db::RedisObject::CreateWithString("two"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"second", "two"}, source.get()));
  ASSERT_TRUE(writer->Flush());
  writer->WaitForRewrite();
  EXPECT_EQ(writer->State().rewrite_status, RewriteStatus::kSucceeded);
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("first"), nullptr);
  EXPECT_EQ(restored->LookupKey("first")->String(), "one");
  ASSERT_NE(restored->LookupKey("second"), nullptr);
  EXPECT_EQ(restored->LookupKey("second")->String(), "two");
}

TEST(AofTest, OpensFileCloseOnExec) {
  // A child started with exec() must not inherit a writable AOF descriptor.
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto db = db::RedisDb::Create();
  auto aof = Aof::Open(Always(file, file_ops), db.get());
  ASSERT_NE(aof, nullptr);
  // The always policy syncs the AOF's own descriptor after each append.
  ASSERT_TRUE(aof->Append("SET", {"key", "value"}, db.get()));
  ASSERT_TRUE(aof->Flush());
  const int flags = file_ops->SyncedFdFlags();
  ASSERT_GE(flags, 0);
  EXPECT_NE(flags & FD_CLOEXEC, 0);
}

TEST(AofTest, ReportsAppendSyncFailures) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file, file_ops), source.get());
  ASSERT_NE(writer, nullptr);

  file_ops->FailNextSync();
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  EXPECT_FALSE(writer->Flush());
  const AofState state = writer->State();
  EXPECT_FALSE(state.accepting_writes);
  EXPECT_EQ(state.last_error, AofError::kSync);
}

TEST(AofTest, ReportsAppendWriteFailures) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file, file_ops), source.get());
  ASSERT_NE(writer, nullptr);

  file_ops->FailWrites();
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  EXPECT_FALSE(writer->Flush());
  EXPECT_FALSE(writer->AcceptingWrites());
  EXPECT_EQ(writer->State().last_error, AofError::kWrite);
}

TEST(AofTest, KeepsOriginalFileWhenRewriteRenameFails) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file, file_ops), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));

  file_ops->FailRenames();
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  const AofState state = writer->State();
  EXPECT_TRUE(state.accepting_writes);
  EXPECT_EQ(state.rewrite_status, RewriteStatus::kFailed);
  EXPECT_EQ(state.last_error, AofError::kRename);

  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("updated"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "updated"}, source.get()));
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "updated");
}

TEST(AofTest, ReportsRewriteWriteFailuresWithoutStoppingAppends) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file, file_ops), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));

  file_ops->FailRewriteWrites();
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  const AofState state = writer->State();
  EXPECT_TRUE(state.accepting_writes);
  EXPECT_EQ(state.rewrite_status, RewriteStatus::kFailed);
  EXPECT_EQ(state.last_error, AofError::kWrite);

  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("updated"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "updated"}, source.get()));
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "updated");
}

TEST(AofTest, ReportsDirectorySyncFailureAfterRewrite) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file, file_ops), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));

  // The server flushes every batch before the next event, so do the same
  // before arming the failure for the rewrite's syncs.
  ASSERT_TRUE(writer->Flush());
  file_ops->FailSyncAfterNext();
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  const AofState state = writer->State();
  EXPECT_FALSE(state.accepting_writes);
  EXPECT_EQ(state.rewrite_status, RewriteStatus::kFailed);
  EXPECT_EQ(state.last_error, AofError::kDirectorySync);
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "value");
}

TEST(AofTest, AbandonsRewriteWhenItsBufferOverflows) {
  TempFile file;
  auto source = db::RedisDb::Create();
  Options options = Always(file);
  options.limits.max_rewrite_buffer_bytes = 16;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));

  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  // A mutation larger than the rewrite buffer abandons the rewrite but still
  // reaches the current file.
  const std::string large(64, 'x');
  ASSERT_EQ(
      source->SetKey("large", db::RedisObject::CreateWithString(large), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"large", large}, source.get()));
  ASSERT_TRUE(writer->Flush());
  const AofState state = writer->State();
  EXPECT_FALSE(state.rewrite_in_progress);
  EXPECT_EQ(state.rewrite_status, RewriteStatus::kFailed);
  EXPECT_EQ(state.last_error, AofError::kRewriteBufferFull);
  EXPECT_TRUE(state.accepting_writes);
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(restored->KeyCount(), 2);
}

TEST(AofTest, WritesTheBatchBeforeFlushReturns) {
  // The reply follows Flush, so a crash of the server after it cannot lose
  // the write, whatever the fsync policy.
  for (const FsyncPolicy policy :
       {FsyncPolicy::kEverySecond, FsyncPolicy::kNo}) {
    TempFile file;
    auto source = db::RedisDb::Create();
    auto writer = Aof::Open({file.Path(), policy}, source.get());
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(
        source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
        db::DbStatus::kOk);
    ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
    ASSERT_TRUE(writer->Flush());
    EXPECT_EQ(file.Size(), Encode("SET", {"key", "value"}).size());
  }
}

TEST(AofTest, SyncsEverySecondUnderSustainedWrites) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options{file.Path(), FsyncPolicy::kEverySecond};
  options.file_ops = file_ops;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  // Constant slow writes kept the old background writer busy enough that it
  // never synced; the sync thread now runs independently of the writes.
  file_ops->SlowWrites(std::chrono::milliseconds(2));
  const int syncs_before = file_ops->SyncCount();
  const auto end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1300);
  while (std::chrono::steady_clock::now() < end) {
    ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
    ASSERT_TRUE(writer->Flush());
  }
  EXPECT_GE(file_ops->SyncCount() - syncs_before, 1);
}

TEST(AofTest, RetriesFailedWritesAndRejectsWritesUntilTheyRecover) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options{file.Path(), FsyncPolicy::kEverySecond};
  options.file_ops = file_ops;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);

  file_ops->FailWrites();
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  // Without `always` the batch is kept, so its replies may still be sent.
  EXPECT_TRUE(writer->Flush());
  EXPECT_FALSE(writer->AcceptingWrites());
  EXPECT_EQ(writer->State().last_error, AofError::kWrite);
  EXPECT_GT(writer->State().pending_bytes, 0);
  EXPECT_FALSE(writer->LastErrorText().empty());

  file_ops->RestoreWrites();
  EXPECT_TRUE(writer->Flush());
  EXPECT_TRUE(writer->AcceptingWrites());
  EXPECT_EQ(writer->State().pending_bytes, 0);
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
}

TEST(AofTest, FailedSyncStopsWritesUntilARewriteReplacesTheFile) {
  TempFile file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options{file.Path(), FsyncPolicy::kEverySecond};
  options.file_ops = file_ops;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);

  file_ops->FailNextSync();
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  // The kernel may have dropped what it could not sync, so retrying the sync
  // proves nothing; the error stays.
  EXPECT_FALSE(writer->SyncNow());
  EXPECT_FALSE(writer->AcceptingWrites());
  EXPECT_FALSE(writer->SyncNow());
  EXPECT_EQ(writer->State().last_error, AofError::kSync);

  // A rewrite writes and syncs a complete new file, which clears it.
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  EXPECT_EQ(writer->State().rewrite_status, RewriteStatus::kSucceeded);
  EXPECT_TRUE(writer->AcceptingWrites());
}

TEST(AofTest, ShutdownStopsARunningRewrite) {
  TempFile file;
  auto source = db::RedisDb::Create();
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer.reset();

  // The temporary file is gone, and the original still holds the data.
  const std::filesystem::path path(file.Path());
  for (const auto& entry :
       std::filesystem::directory_iterator(path.parent_path())) {
    EXPECT_FALSE(entry.path().filename().string().starts_with(
        path.filename().string() + ".rewrite."))
        << entry.path();
  }
  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
}

TEST(AofTest, BatchesAppendsUntilFlush) {
  TempFile file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);

  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  EXPECT_EQ(file.Size(), 0);
  EXPECT_GT(writer->State().pending_bytes, 0);
  ASSERT_TRUE(writer->Flush());
  EXPECT_GT(file.Size(), 0);
  EXPECT_EQ(writer->State().pending_bytes, 0);
}

TEST(AofTest, DetectsWhenAutomaticRewriteIsDue) {
  TempFile file;
  auto source = db::RedisDb::Create();
  Options options = Always(file);
  options.auto_rewrite_min_bytes = 0;
  options.auto_rewrite_percentage = 1;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  EXPECT_FALSE(writer->ShouldAutoRewrite());

  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  EXPECT_TRUE(writer->ShouldAutoRewrite());
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  EXPECT_FALSE(writer->ShouldAutoRewrite());
  EXPECT_EQ(writer->State().rewrite_status, RewriteStatus::kSucceeded);
}

TEST(AofTest, ReplaysCompleteTransactions) {
  TempFile file;
  std::string commands = Encode("SET", {"before", "1"});
  commands.append(Encode("MULTI", {}));
  commands.append(Encode("SET", {"counter", "1"}));
  commands.append(Encode("INCR", {"counter"}));
  commands.append(Encode("EXEC", {}));
  ASSERT_TRUE(file.Append(commands));

  auto restored = db::RedisDb::Create();
  auto aof = Aof::Open(Always(file), restored.get());

  ASSERT_NE(aof, nullptr);
  EXPECT_EQ(file.Size(), static_cast<off_t>(commands.size()));
  ASSERT_NE(restored->LookupKey("before"), nullptr);
  ASSERT_NE(restored->LookupKey("counter"), nullptr);
  EXPECT_EQ(restored->LookupKey("counter")->String(), "2");
}

TEST(AofTest, DiscardsTransactionCutOffByEndOfFile) {
  TempFile file;
  const std::string complete = Encode("SET", {"before", "1"});
  std::string commands = complete;
  commands.append(Encode("MULTI", {}));
  commands.append(Encode("SET", {"first", "1"}));
  commands.append(Encode("SET", {"second", "2"}));
  // The EXEC record never reached the file.
  ASSERT_TRUE(file.Append(commands));

  auto restored = db::RedisDb::Create();
  auto aof = Aof::Open(Always(file), restored.get());

  ASSERT_NE(aof, nullptr);
  EXPECT_EQ(file.Size(), static_cast<off_t>(complete.size()));
  EXPECT_NE(restored->LookupKey("before"), nullptr);
  EXPECT_EQ(restored->LookupKey("first"), nullptr);
  EXPECT_EQ(restored->LookupKey("second"), nullptr);
}

TEST(AofTest, RejectsUnbalancedTransactionRecords) {
  for (const std::string& contents :
       {Encode("EXEC", {}), Encode("MULTI", {}) + Encode("MULTI", {}),
        Encode("MULTI", {}) + Encode("GET", {"key"}) + Encode("EXEC", {})}) {
    TempFile file;
    ASSERT_TRUE(file.Append(contents));
    auto database = db::RedisDb::Create();
    EXPECT_EQ(Aof::Open(Always(file), database.get()), nullptr);
  }
}

TEST(AofTest, WrapsMultiRecordTransactions) {
  TempFile file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(source->SetKey("a", db::RedisObject::CreateWithString("1"), 0),
            db::DbStatus::kOk);
  ASSERT_EQ(source->SetKey("b", db::RedisObject::CreateWithString("2"), 0),
            db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);

  // A single mutation is already atomic and stays unwrapped.
  writer->BeginTransaction();
  ASSERT_TRUE(writer->Append("SET", {"a", "1"}, source.get()));
  writer->EndTransaction();
  ASSERT_TRUE(writer->Append("DEL", {"missing"}, source.get()));
  writer->BeginTransaction();
  ASSERT_TRUE(writer->Append("SET", {"a", "1"}, source.get()));
  ASSERT_TRUE(writer->Append("SET", {"b", "2"}, source.get()));
  writer->EndTransaction();
  ASSERT_TRUE(writer->Flush());
  writer.reset();

  std::string expected = Encode("SET", {"a", "1"});
  expected.append(Encode("DEL", {"missing"}));
  expected.append(Encode("MULTI", {}));
  expected.append(Encode("SET", {"a", "1"}));
  expected.append(Encode("SET", {"b", "2"}));
  expected.append(Encode("EXEC", {}));
  std::ifstream input(file.Path(), std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, expected);

  auto restored = db::RedisDb::Create();
  EXPECT_NE(Aof::Open(Always(file), restored.get()), nullptr);
  EXPECT_EQ(restored->KeyCount(), 2);
}

TEST(AofTest, FlushInsideTransactionSplitsItIntoAtomicSegments) {
  TempFile file;
  auto source = db::RedisDb::Create();
  for (const std::string_view key : {"a", "b", "c", "d"}) {
    ASSERT_EQ(source->SetKey(key, db::RedisObject::CreateWithString("v"), 0),
              db::DbStatus::kOk);
  }
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);

  writer->BeginTransaction();
  ASSERT_TRUE(writer->Append("SET", {"a", "v"}, source.get()));
  ASSERT_TRUE(writer->Append("SET", {"b", "v"}, source.get()));
  // StartRewrite flushes the pending batch, as BGREWRITEAOF inside EXEC does.
  ASSERT_TRUE(writer->Flush());
  ASSERT_TRUE(writer->Append("SET", {"c", "v"}, source.get()));
  ASSERT_TRUE(writer->Append("SET", {"d", "v"}, source.get()));
  writer->EndTransaction();
  ASSERT_TRUE(writer->Flush());
  writer.reset();

  std::string expected;
  for (const auto& keys : {std::array<std::string_view, 2>{"a", "b"},
                           std::array<std::string_view, 2>{"c", "d"}}) {
    expected.append(Encode("MULTI", {}));
    expected.append(Encode("SET", {keys[0], "v"}));
    expected.append(Encode("SET", {keys[1], "v"}));
    expected.append(Encode("EXEC", {}));
  }
  std::ifstream input(file.Path(), std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, expected);
}
}  // namespace redis_simple::aof
