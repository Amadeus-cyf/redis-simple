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
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/reply.h"
#include "utils/check.h"
#include "utils/time_utils.h"

namespace redis_simple::aof {
namespace {
// A temporary directory holding an AOF directory, removed afterwards.
class TempAof {
 public:
  TempAof() {
    constexpr std::string_view kTemplate = "/tmp/redis_simple_aof_XXXXXX";
    std::array<char, kTemplate.size() + 1> path{};
    std::ranges::copy(kTemplate, path.begin());
    RS_CHECK(mkdtemp(path.data()) != nullptr);
    root_ = path.data();
    directory_ = root_ + "/appendonlydir";
  }
  TempAof(const TempAof&) = delete;
  TempAof& operator=(const TempAof&) = delete;
  TempAof(TempAof&&) = delete;
  TempAof& operator=(TempAof&&) = delete;
  ~TempAof() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  [[nodiscard]] const std::string& Root() const { return root_; }
  [[nodiscard]] const std::string& Directory() const { return directory_; }
  [[nodiscard]] Options MakeOptions(FsyncPolicy policy) const {
    return {directory_, std::string(kFileName), policy};
  }
  [[nodiscard]] std::string ManifestPath() const {
    return directory_ + "/" + std::string(kFileName) + ".manifest";
  }

  // The manifest's base and incremental files, in replay order.
  [[nodiscard]] std::vector<std::string> Files() const {
    std::vector<std::string> paths;
    std::ifstream input(ManifestPath(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const std::optional<Manifest> manifest = Manifest::Parse(text);
    if (!manifest.has_value()) {
      return paths;
    }
    if (manifest->base.has_value()) {
      paths.push_back(directory_ + "/" + manifest->base->name);
    }
    for (const ManifestFile& file : manifest->increments) {
      paths.push_back(directory_ + "/" + file.name);
    }
    return paths;
  }

  // Appends to the last file, as a write cut off by a crash would. Without a
  // manifest yet, creates one listing a single incremental file.
  [[nodiscard]] bool Append(std::string_view data) const {
    std::vector<std::string> files = Files();
    if (files.empty()) {
      std::filesystem::create_directories(directory_);
      std::ofstream manifest(ManifestPath(), std::ios::binary);
      manifest << "file " << kFileName << ".1.incr.aof seq 1 type i\n";
      if (!manifest) {
        return false;
      }
      files.push_back(directory_ + "/" + std::string(kFileName) +
                      ".1.incr.aof");
    }
    std::ofstream output(files.back(), std::ios::binary | std::ios::app);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(output);
  }

  // Bytes in the base and incremental files together.
  [[nodiscard]] off_t Size() const {
    off_t total = 0;
    for (const std::string& path : Files()) {
      struct stat info {};
      if (stat(path.c_str(), &info) != 0) {
        return -1;
      }
      total += info.st_size;
    }
    return total;
  }

  // The base and incremental files' contents, concatenated.
  [[nodiscard]] std::string Contents() const {
    std::string contents;
    for (const std::string& path : Files()) {
      std::ifstream input(path, std::ios::binary);
      contents.append(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>());
    }
    return contents;
  }

 private:
  static constexpr std::string_view kFileName = "appendonly.aof";
  std::string root_;
  std::string directory_;
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
  void RestoreRewriteWrites() { fail_write_vector_.store(false); }
  void FailNextSync() { fail_sync_call_.store(sync_calls_.load() + 1); }
  void FailSyncAfterNext() { fail_sync_call_.store(sync_calls_.load() + 2); }
  void FailRenames() { fail_rename_.store(true); }
  void RestoreRenames() { fail_rename_.store(false); }
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

Options Always(const TempAof& file) {
  return file.MakeOptions(FsyncPolicy::kAlways);
}

Options Always(const TempAof& file, const std::shared_ptr<FileOps>& file_ops) {
  Options options = Always(file);
  options.file_ops = file_ops;
  return options;
}
}  // namespace

TEST(AofTest, ReplaysCanonicalSetAndExpiration) {
  TempAof file;
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
  ASSERT_TRUE(file.Append("not-resp\r\n"));
  auto database = db::RedisDb::Create();

  EXPECT_EQ(Aof::Open(Always(file), database.get()), nullptr);
}

TEST(AofTest, FlushesThePendingBatchOnShutdown) {
  TempAof file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  Options options = file.MakeOptions(FsyncPolicy::kNo);
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  file_ops->LimitWrites(3);
  auto source = db::RedisDb::Create();
  Options options = file.MakeOptions(FsyncPolicy::kNo);
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file, file_ops), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));

  // Starting needs a manifest rename too: with renames failing, the rewrite
  // does not start, and appends stay in the current file.
  file_ops->FailRenames();
  EXPECT_EQ(writer->StartRewrite(source.get()), RewriteResult::kError);
  EXPECT_EQ(writer->CurrentManifest().increments.size(), 1);
  file_ops->RestoreRenames();

  // A failed rename of the new base keeps the old manifest.
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  file_ops->FailRenames();
  writer->WaitForRewrite();
  file_ops->RestoreRenames();
  const AofState state = writer->State();
  EXPECT_TRUE(state.accepting_writes);
  EXPECT_EQ(state.rewrite_status, RewriteStatus::kFailed);
  EXPECT_EQ(state.last_error, AofError::kRename);
  EXPECT_EQ(state.consecutive_rewrite_failures, 2);

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
  TempAof file;
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
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  auto writer = Aof::Open(Always(file, file_ops), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));

  ASSERT_TRUE(writer->Flush());
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  // Installing the rewrite syncs the new manifest, then the directory after
  // renaming it; the directory sync fails. The child's syncs happen in its
  // own process and do not count.
  file_ops->FailSyncAfterNext();
  writer->WaitForRewrite();
  const AofState state = writer->State();
  EXPECT_FALSE(state.accepting_writes);
  EXPECT_EQ(state.rewrite_status, RewriteStatus::kFailed);
  EXPECT_EQ(state.last_error, AofError::kDirectorySync);
  // The renamed manifest is live, but the files it replaced stay until a
  // later manifest is safely on disk, in case the old one comes back.
  EXPECT_FALSE(writer->CurrentManifest().history.empty());
  for (const ManifestFile& history : writer->CurrentManifest().history) {
    EXPECT_TRUE(std::filesystem::exists(file.Directory() + "/" + history.name))
        << history.name;
  }
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "value");
}

TEST(AofTest, RewriteStartsANewIncrementFileAndRetiresTheOldFiles) {
  TempAof file;
  auto source = db::RedisDb::Create();
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(writer->CurrentManifest().increments.size(), 1);
  EXPECT_FALSE(writer->CurrentManifest().base.has_value());
  ASSERT_EQ(source->SetKey("a", db::RedisObject::CreateWithString("1"), 0),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"a", "1"}, source.get()));

  // Starting a rewrite switches appends to a new incremental file at once;
  // writes made during the rewrite go there, not to a buffer.
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  ASSERT_EQ(writer->CurrentManifest().increments.size(), 2);
  const std::string old_increment =
      writer->CurrentManifest().increments.front().name;
  ASSERT_EQ(source->SetKey("b", db::RedisObject::CreateWithString("2"), 0),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"b", "2"}, source.get()));
  ASSERT_TRUE(writer->Flush());
  writer->WaitForRewrite();
  EXPECT_EQ(writer->State().rewrite_status, RewriteStatus::kSucceeded);

  // The new base and the file opened at the start remain; the old
  // incremental file is deleted.
  const Manifest& manifest = writer->CurrentManifest();
  ASSERT_TRUE(manifest.base.has_value());
  EXPECT_EQ(manifest.base->name, "appendonly.aof.1.base.aof");
  ASSERT_EQ(manifest.increments.size(), 1);
  EXPECT_EQ(manifest.increments.front().name, "appendonly.aof.2.incr.aof");
  EXPECT_TRUE(manifest.history.empty());
  EXPECT_FALSE(std::filesystem::exists(file.Directory() + "/" + old_increment));
  writer.reset();

  std::ifstream input(file.ManifestPath(), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(text,
            "file appendonly.aof.1.base.aof seq 1 type b\n"
            "file appendonly.aof.2.incr.aof seq 2 type i\n");
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
    TempAof file;
    auto source = db::RedisDb::Create();
    auto writer = Aof::Open(file.MakeOptions(policy), source.get());
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
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options = file.MakeOptions(FsyncPolicy::kEverySecond);
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
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options = file.MakeOptions(FsyncPolicy::kEverySecond);
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
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options = file.MakeOptions(FsyncPolicy::kEverySecond);
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
  TempAof file;
  auto source = db::RedisDb::Create();
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer.reset();

  // The temporary base is gone, and the files still hold the data.
  for (const auto& entry :
       std::filesystem::directory_iterator(file.Directory())) {
    EXPECT_FALSE(entry.path().filename().string().starts_with("temp-"))
        << entry.path();
  }
  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
}

TEST(AofTest, BatchesAppendsUntilFlush) {
  TempAof file;
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
  TempAof file;
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
  TempAof file;
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
  TempAof file;
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
    TempAof file;
    ASSERT_TRUE(file.Append(contents));
    auto database = db::RedisDb::Create();
    EXPECT_EQ(Aof::Open(Always(file), database.get()), nullptr);
  }
}

TEST(AofTest, WrapsMultiRecordTransactions) {
  TempAof file;
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
  const std::string contents = file.Contents();
  EXPECT_EQ(contents, expected);

  auto restored = db::RedisDb::Create();
  EXPECT_NE(Aof::Open(Always(file), restored.get()), nullptr);
  EXPECT_EQ(restored->KeyCount(), 2);
}

TEST(AofTest, FlushInsideTransactionSplitsItIntoAtomicSegments) {
  TempAof file;
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
  const std::string contents = file.Contents();
  EXPECT_EQ(contents, expected);
}

TEST(ManifestTest, EncodesAndParsesRedisFormat) {
  Manifest manifest;
  manifest.base =
      ManifestFile{"appendonly.aof.3.base.aof", 3, ManifestFile::Type::kBase};
  manifest.history.push_back(ManifestFile{"appendonly.aof.2.base.aof", 2,
                                          ManifestFile::Type::kHistory});
  manifest.increments.push_back(ManifestFile{"appendonly.aof.5.incr.aof", 5,
                                             ManifestFile::Type::kIncrement});
  manifest.increments.push_back(ManifestFile{"appendonly.aof.6.incr.aof", 6,
                                             ManifestFile::Type::kIncrement});
  const std::string text = manifest.Encode();
  EXPECT_EQ(text,
            "file appendonly.aof.3.base.aof seq 3 type b\n"
            "file appendonly.aof.2.base.aof seq 2 type h\n"
            "file appendonly.aof.5.incr.aof seq 5 type i\n"
            "file appendonly.aof.6.incr.aof seq 6 type i\n");

  const std::optional<Manifest> parsed = Manifest::Parse(text);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->base.has_value());
  EXPECT_EQ(parsed->base->name, "appendonly.aof.3.base.aof");
  ASSERT_EQ(parsed->increments.size(), 2);
  EXPECT_EQ(parsed->increments.back().sequence, 6);
  ASSERT_EQ(parsed->history.size(), 1);
  EXPECT_EQ(parsed->base_sequence, 3);
  EXPECT_EQ(parsed->increment_sequence, 6);
}

TEST(ManifestTest, AcceptsAnyKeyOrderCommentsAndUnknownKeys) {
  const std::optional<Manifest> parsed = Manifest::Parse(
      "# written by a newer version\n"
      "type b seq 1 file a.1.base.rdb extra x\r\n"
      "\n"
      "file a.4.incr.aof seq 4 type i\n");
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->base.has_value());
  EXPECT_EQ(parsed->base->name, "a.1.base.rdb");
  ASSERT_EQ(parsed->increments.size(), 1);
}

TEST(ManifestTest, RejectsMalformedManifests) {
  for (const std::string_view text : {
           "file a seq 1\n",                              // no type
           "file a type b\n",                             // no sequence
           "seq 1 type b\n",                              // no file
           "file a seq x type b\n",                       // bad sequence
           "file a seq 1 type z\n",                       // bad type
           "file dir/a seq 1 type b\n",                   // a directory
           "file a seq 1 type\n",                         // missing value
           "file a seq 1 type b\nfile c seq 2 type b\n",  // two bases
           "file a seq 2 type i\nfile c seq 1 type i\n",  // out of order
       }) {
    EXPECT_FALSE(Manifest::Parse(text).has_value()) << text;
  }
}

TEST(AofTest, UpgradesASingleFileAof) {
  TempAof file;
  // Like Redis, the old file is looked up in the working directory.
  const std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::current_path(file.Root());
  {
    std::ofstream legacy("appendonly.aof", std::ios::binary);
    legacy << Encode("SET", {"key", "old"});
  }
  Options options = file.MakeOptions(FsyncPolicy::kAlways);
  options.directory = "appendonlydir";
  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(options, restored.get());
  std::filesystem::current_path(previous);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "old");
  // It moved into the directory as the base file, keeping its name.
  EXPECT_FALSE(std::filesystem::exists(file.Root() + "/appendonly.aof"));
  ASSERT_TRUE(reader->CurrentManifest().base.has_value());
  EXPECT_EQ(reader->CurrentManifest().base->name, "appendonly.aof");
  EXPECT_TRUE(std::filesystem::exists(file.Directory() + "/appendonly.aof"));
  EXPECT_EQ(reader->CurrentManifest().increments.size(), 1);
}

TEST(AofTest, KeepsAppendingToTheLastIncrementAcrossRestarts) {
  TempAof file;
  for (int round = 0; round < 3; ++round) {
    auto db = db::RedisDb::Create();
    auto writer = Aof::Open(Always(file), db.get());
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(writer->CurrentManifest().increments.size(), 1);
    EXPECT_EQ(writer->CurrentManifest().increments.front().sequence, 1);
    const std::string value = std::to_string(round);
    ASSERT_EQ(db->SetKey("key", db::RedisObject::CreateWithString(value), 0),
              db::DbStatus::kOk);
    ASSERT_TRUE(writer->Append("SET", {"key", value}, db.get()));
  }
  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(restored->LookupKey("key")->String(), "2");
}

TEST(AofTest, OnlyTheLastFileMayEndInATornRecord) {
  TempAof file;
  {
    auto db = db::RedisDb::Create();
    auto writer = Aof::Open(Always(file), db.get());
    ASSERT_NE(writer, nullptr);
    ASSERT_EQ(db->SetKey("a", db::RedisObject::CreateWithString("1"), 0),
              db::DbStatus::kOk);
    ASSERT_TRUE(writer->Append("SET", {"a", "1"}, db.get()));
    ASSERT_EQ(writer->StartRewrite(db.get()), RewriteResult::kStarted);
    writer->WaitForRewrite();
    ASSERT_EQ(db->SetKey("b", db::RedisObject::CreateWithString("2"), 0),
              db::DbStatus::kOk);
    ASSERT_TRUE(writer->Append("SET", {"b", "2"}, db.get()));
  }
  const std::vector<std::string> files = file.Files();
  ASSERT_EQ(files.size(), 2);
  const std::string torn = "*2\r\n$3\r\nDEL\r\n$1\r";

  // A torn tail on the last file is cut off.
  ASSERT_TRUE(file.Append(torn));
  {
    auto restored = db::RedisDb::Create();
    ASSERT_NE(Aof::Open(Always(file), restored.get()), nullptr);
    EXPECT_EQ(restored->KeyCount(), 2);
  }
  // On an earlier file it means lost data, so loading fails.
  {
    std::ofstream base(files.front(), std::ios::binary | std::ios::app);
    base << torn;
  }
  auto restored = db::RedisDb::Create();
  EXPECT_EQ(Aof::Open(Always(file), restored.get()), nullptr);
}

TEST(AofTest, BacksOffAutomaticRewritesAfterRepeatedFailures) {
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options = Always(file, file_ops);
  options.auto_rewrite_min_bytes = 1;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(source->SetKey("key", db::RedisObject::CreateWithString("v"), 0),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "v"}, source.get()));
  ASSERT_TRUE(writer->Flush());

  file_ops->FailRewriteWrites();
  for (size_t attempt = 1; attempt <= 3; ++attempt) {
    EXPECT_TRUE(writer->ShouldAutoRewrite()) << attempt;
    ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
    writer->WaitForRewrite();
    EXPECT_EQ(writer->State().consecutive_rewrite_failures, attempt);
  }
  // Three failures in a row: automatic rewrites wait a minute, but an
  // explicit BGREWRITEAOF still runs.
  EXPECT_FALSE(writer->ShouldAutoRewrite());
  file_ops->RestoreRewriteWrites();
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  EXPECT_EQ(writer->State().rewrite_status, RewriteStatus::kSucceeded);
  EXPECT_EQ(writer->State().consecutive_rewrite_failures, 0);
}

TEST(AofTest, SyncFailureOnTheFileARewriteKeepsStays) {
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options = file.MakeOptions(FsyncPolicy::kEverySecond);
  options.file_ops = file_ops;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  // The sync fails on the incremental file opened for the rewrite, which
  // the rewrite keeps, so its success cannot clear the error.
  ASSERT_EQ(source->SetKey("key", db::RedisObject::CreateWithString("v"), 0),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "v"}, source.get()));
  file_ops->FailNextSync();
  EXPECT_FALSE(writer->SyncNow());
  writer->WaitForRewrite();
  EXPECT_EQ(writer->State().rewrite_status, RewriteStatus::kSucceeded);
  EXPECT_FALSE(writer->AcceptingWrites());
}
}  // namespace redis_simple::aof
