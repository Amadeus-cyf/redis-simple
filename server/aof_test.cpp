#include "server/aof.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "server/db/db.h"
#include "server/db/redis_obj.h"
#include "server/reply.h"
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
    if (fd < 0) {
      throw std::runtime_error("failed to create temporary AOF");
    }
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
  void FailWrites() { fail_write_.store(true); }
  void FailRewriteWrites() { fail_write_vector_.store(true); }
  void FailNextSync() { fail_sync_call_.store(sync_calls_.load() + 1); }
  void FailSyncAfterNext() { fail_sync_call_.store(sync_calls_.load() + 2); }
  void FailRenames() { fail_rename_.store(true); }

 private:
  size_t max_write_size_{std::numeric_limits<size_t>::max()};
  std::atomic<bool> fail_write_;
  std::atomic<bool> fail_write_vector_;
  std::atomic<bool> fail_rename_;
  std::atomic<int> sync_calls_;
  std::atomic<int> fail_sync_call_{-1};
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
  const auto* object = restored->LookupKey("key");
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

TEST(AofTest, FlushesBackgroundWritesOnShutdown) {
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
  writer->WaitUntilIdle();
  EXPECT_TRUE(writer->Healthy());
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
  writer->WaitUntilRewriteIdle();
  ASSERT_TRUE(writer->Healthy());
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
  writer->WaitUntilRewriteIdle();

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
  options.limits.max_snapshot_queue_bytes = 192;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitUntilRewriteIdle();
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
  options.limits.max_snapshot_queue_bytes = 256;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitUntilRewriteIdle();
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(options, restored.get());
  ASSERT_NE(reader, nullptr);
  const auto* object = restored->LookupKey("list");
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

TEST(AofTest, HandlesShortVectorWritesWithoutLosingCommands) {
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
  ASSERT_EQ(
      source->SetKey("second", db::RedisObject::CreateWithString("two"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"second", "two"}, source.get()));
  writer->WaitUntilIdle();
  ASSERT_TRUE(writer->Healthy());
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("first"), nullptr);
  EXPECT_EQ(restored->LookupKey("first")->String(), "one");
  ASSERT_NE(restored->LookupKey("second"), nullptr);
  EXPECT_EQ(restored->LookupKey("second")->String(), "two");
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
  const auto state = writer->State();
  EXPECT_FALSE(state.healthy);
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
  EXPECT_FALSE(writer->Healthy());
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
  writer->WaitUntilRewriteIdle();
  const auto state = writer->State();
  EXPECT_TRUE(state.healthy);
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
  writer->WaitUntilRewriteIdle();
  const auto state = writer->State();
  EXPECT_TRUE(state.healthy);
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
  writer->WaitUntilRewriteIdle();
  const auto state = writer->State();
  EXPECT_FALSE(state.healthy);
  EXPECT_EQ(state.rewrite_status, RewriteStatus::kFailed);
  EXPECT_EQ(state.last_error, AofError::kDirectorySync);
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "value");
}

TEST(AofTest, AppliesBackpressureWhenTheBackgroundQueueIsFull) {
  TempFile file;
  auto source = db::RedisDb::Create();
  Options options{file.Path(), FsyncPolicy::kNo};
  options.limits.max_pending_commands = 1;
  options.limits.max_pending_bytes = 1;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);

  constexpr int kBatches = 64;
  for (int i = 0; i < kBatches; ++i) {
    const std::string key = "key" + std::to_string(i);
    ASSERT_EQ(
        source->SetKey(key, db::RedisObject::CreateWithString("value"), 0),
        db::DbStatus::kOk);
    ASSERT_TRUE(writer->Append("SET", {key, "value"}, source.get()));
    // Each flush waits for the writer instead of failing the AOF.
    ASSERT_TRUE(writer->Flush());
  }
  writer->WaitUntilIdle();
  EXPECT_TRUE(writer->Healthy());
  EXPECT_EQ(writer->State().last_error, AofError::kNone);
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(restored->KeyCount(), kBatches);
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
  writer->WaitUntilRewriteIdle();
  EXPECT_FALSE(writer->ShouldAutoRewrite());
  EXPECT_EQ(writer->State().rewrite_status, RewriteStatus::kSucceeded);
}
}  // namespace redis_simple::aof
