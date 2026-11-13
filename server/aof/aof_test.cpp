#include "server/aof/aof.h"

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
#include "server/rdb/rdb_writer.h"
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
  // Options with a command-format base file, which most tests read back
  // and whose rewrite writes the test file operations tell apart; the RDB
  // base file has tests of its own.
  [[nodiscard]] Options MakeOptions(FsyncPolicy policy) const {
    Options options{directory_, std::string(kFileName), policy};
    options.use_rdb_preamble = false;
    return options;
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

  [[nodiscard]] std::string ManifestText() const {
    std::ifstream input(ManifestPath(), std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }

  // Bytes in the base and incremental files together.
  [[nodiscard]] off_t Size() const {
    off_t total = 0;
    for (const std::string& path : Files()) {
      struct stat info{};
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
    if (writable_bytes_.has_value()) {
      // A disk that fills up partway through a write.
      if (*writable_bytes_ == 0) {
        errno = ENOSPC;
        return -1;
      }
      size = std::min(size, *writable_bytes_);
      *writable_bytes_ -= size;
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
    std::this_thread::sleep_for(sync_delay_.load());
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
  void RestoreWrites() {
    fail_write_.store(false);
    writable_bytes_.reset();
  }
  // Lets this many more bytes through, then fails writes with ENOSPC.
  void FailWritesAfter(size_t bytes) { writable_bytes_ = bytes; }
  void SlowSyncs(std::chrono::milliseconds delay) { sync_delay_.store(delay); }
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
  std::optional<size_t> writable_bytes_;
  std::atomic<std::chrono::milliseconds> sync_delay_{
      std::chrono::milliseconds(0)};
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

// Redis selects the database before the first command of each file.
constexpr std::string_view kSelect = "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n";

Options Always(const TempAof& file) {
  return file.MakeOptions(FsyncPolicy::kAlways);
}

Options Always(const TempAof& file, const std::shared_ptr<FileOps>& file_ops) {
  Options options = Always(file);
  options.file_ops = file_ops;
  return options;
}

// Options writing an RDB base file, Redis' default.
Options WithRdbPreamble(const TempAof& file) {
  Options options = Always(file);
  options.use_rdb_preamble = true;
  return options;
}

// `db` as an AOF base RDB file's bytes, written next to the AOF directory.
std::string RdbBytes(db::RedisDb* db, const TempAof& file) {
  const std::string path = file.Root() + "/snapshot.rdb";
  if (rdb::WriteRdbFile(db, path, {.aof_base = true}, DefaultFileOps().get()) !=
      rdb::WriteStatus::kWritten) {
    return {};
  }
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

// Writes an old single-file AOF, which Redis may have started with an RDB
// preamble, where Aof::Open looks for one, and opens it.
std::unique_ptr<Aof> OpenSingleFile(const TempAof& file, std::string_view data,
                                    db::RedisDb* db) {
  const std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::current_path(file.Root());
  {
    std::ofstream legacy("appendonly.aof", std::ios::binary);
    legacy.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  Options options = WithRdbPreamble(file);
  options.directory = "appendonlydir";
  std::unique_ptr<Aof> aof = Aof::Open(options, db);
  std::filesystem::current_path(previous);
  return aof;
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
  EXPECT_TRUE(writer->State().last_rewrite_ok);
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
  EXPECT_TRUE(writer->State().last_rewrite_ok);
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

  // With `always`, a failed sync stops the server, as Redis exits.
  file_ops->FailNextSync();
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  EXPECT_FALSE(writer->Flush());
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
  // does not start, and appends stay in the current file. As in Redis, a
  // rewrite that never forked does not count as a consecutive failure.
  file_ops->FailRenames();
  EXPECT_EQ(writer->StartRewrite(source.get()), RewriteResult::kError);
  EXPECT_EQ(writer->CurrentManifest().increments.size(), 1);
  EXPECT_FALSE(writer->State().last_rewrite_ok);
  EXPECT_EQ(writer->State().consecutive_rewrite_failures, 0);
  file_ops->RestoreRenames();

  // A failed rename of the new base keeps the old manifest.
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  file_ops->FailRenames();
  writer->WaitForRewrite();
  file_ops->RestoreRenames();
  const AofState state = writer->State();
  EXPECT_TRUE(state.accepting_writes);
  EXPECT_FALSE(state.last_rewrite_ok);
  EXPECT_EQ(state.consecutive_rewrite_failures, 1);

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
  EXPECT_FALSE(state.last_rewrite_ok);
  EXPECT_EQ(state.consecutive_rewrite_failures, 1);

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
  // Let the background thread sync and close the replaced file first.
  writer->WaitForBackgroundJobs();
  // Installing the rewrite syncs the new manifest, then the directory after
  // renaming it; the directory sync fails. The child's syncs happen in its
  // own process and do not count.
  file_ops->FailSyncAfterNext();
  writer->WaitForRewrite();
  const AofState state = writer->State();
  EXPECT_FALSE(state.accepting_writes);
  EXPECT_FALSE(state.last_rewrite_ok);
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
  // As in Redis, an AOF with no files starts with an (empty) base file.
  EXPECT_EQ(file.ManifestText(),
            "file appendonly.aof.1.base.aof seq 1 type b\n"
            "file appendonly.aof.1.incr.aof seq 1 type i\n");
  ASSERT_EQ(source->SetKey("a", db::RedisObject::CreateWithString("1"), 0),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"a", "1"}, source.get()));

  // Starting a rewrite switches appends to a new incremental file at once;
  // writes made during the rewrite go there, not to a buffer.
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  ASSERT_EQ(writer->CurrentManifest().increments.size(), 2);
  ASSERT_EQ(source->SetKey("b", db::RedisObject::CreateWithString("2"), 0),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"b", "2"}, source.get()));
  ASSERT_TRUE(writer->Flush());
  writer->WaitForRewrite();
  EXPECT_TRUE(writer->State().last_rewrite_ok);

  // The new base and the file opened at the start remain; base and
  // incremental files are numbered separately, as in Redis. The background
  // thread deletes the retired files.
  EXPECT_TRUE(writer->CurrentManifest().history.empty());
  writer->WaitForBackgroundJobs();
  for (const std::string_view retired :
       {"appendonly.aof.1.base.aof", "appendonly.aof.1.incr.aof"}) {
    EXPECT_FALSE(
        std::filesystem::exists(file.Directory() + "/" + std::string(retired)))
        << retired;
  }
  writer.reset();
  EXPECT_EQ(file.ManifestText(),
            "file appendonly.aof.2.base.aof seq 2 type b\n"
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
    EXPECT_EQ(file.Size(),
              kSelect.size() + Encode("SET", {"key", "value"}).size());
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
  EXPECT_GT(writer->State().buffer_length, 0);
  EXPECT_FALSE(writer->LastErrorText().empty());

  file_ops->RestoreWrites();
  EXPECT_TRUE(writer->Flush());
  EXPECT_TRUE(writer->AcceptingWrites());
  EXPECT_EQ(writer->State().buffer_length, 0);
  writer.reset();

  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(Always(file), restored.get());
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
}

TEST(AofTest, AFailedBackgroundSyncClearsWithTheNextSuccessfulOne) {
  // As in Redis, the last background sync's result decides whether writes
  // are rejected.
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

  for (const bool fail : {true, false}) {
    ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
    ASSERT_TRUE(writer->Flush());
    // `everysec` queues a sync once a second has passed.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    writer->WaitForBackgroundJobs();
    if (fail) {
      file_ops->FailNextSync();
    }
    ASSERT_TRUE(writer->Flush());
    writer->WaitForBackgroundJobs();
    EXPECT_EQ(writer->AcceptingWrites(), !fail);
  }
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

  // The base file created at startup already holds the key.
  const off_t initial_size = file.Size();
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  EXPECT_EQ(file.Size(), initial_size);
  EXPECT_GT(writer->State().buffer_length, 0);
  ASSERT_TRUE(writer->Flush());
  EXPECT_GT(file.Size(), initial_size);
  EXPECT_EQ(writer->State().buffer_length, 0);
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
  // Only written bytes count, as in Redis.
  EXPECT_FALSE(writer->ShouldAutoRewrite());
  ASSERT_TRUE(writer->Flush());
  EXPECT_TRUE(writer->ShouldAutoRewrite());
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  // The new base size is the base plus the current incremental file.
  EXPECT_FALSE(writer->ShouldAutoRewrite());
  EXPECT_TRUE(writer->State().last_rewrite_ok);
  EXPECT_EQ(writer->State().base_size, writer->State().current_size);
}

TEST(AofTest, StartsAScheduledRewriteFromTheCron) {
  TempAof file;
  auto source = db::RedisDb::Create();
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);
  writer->ScheduleRewrite();
  EXPECT_TRUE(writer->State().rewrite_scheduled);
  EXPECT_FALSE(writer->RewriteInProgress());
  ASSERT_TRUE(writer->Cron(source.get()));
  EXPECT_FALSE(writer->State().rewrite_scheduled);
  writer->WaitForRewrite();
  EXPECT_TRUE(writer->State().last_rewrite_ok);
  EXPECT_EQ(writer->State().rewrites, 1);
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
  auto writer = Aof::Open(Always(file), source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(source->SetKey("a", db::RedisObject::CreateWithString("1"), 0),
            db::DbStatus::kOk);
  ASSERT_EQ(source->SetKey("b", db::RedisObject::CreateWithString("2"), 0),
            db::DbStatus::kOk);

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

  std::string expected(kSelect);
  expected.append(Encode("SET", {"a", "1"}));
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
  EXPECT_EQ(reader->CurrentManifest().base.value_or(ManifestFile{}).name,
            "appendonly.aof");
  EXPECT_TRUE(std::filesystem::exists(file.Directory() + "/appendonly.aof"));
  EXPECT_EQ(reader->CurrentManifest().increments.size(), 1);
}

TEST(AofTest, FinishesAnUpgradeCutOffAfterItsManifest) {
  // Redis writes the manifest before moving the old file, so a crash in
  // between repeats the move on the next start.
  TempAof file;
  const std::filesystem::path previous = std::filesystem::current_path();
  std::filesystem::current_path(file.Root());
  {
    std::ofstream legacy("appendonly.aof", std::ios::binary);
    legacy << Encode("SET", {"key", "old"});
  }
  std::filesystem::create_directories(file.Directory());
  {
    std::ofstream manifest(file.ManifestPath(), std::ios::binary);
    manifest << "file appendonly.aof seq 1 type b\n";
  }
  Options options = file.MakeOptions(FsyncPolicy::kAlways);
  options.directory = "appendonlydir";
  auto restored = db::RedisDb::Create();
  auto reader = Aof::Open(options, restored.get());
  std::filesystem::current_path(previous);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_TRUE(std::filesystem::exists(file.Directory() + "/appendonly.aof"));
}

TEST(AofTest, AListedFileThatIsMissingIsFatal) {
  TempAof file;
  ASSERT_TRUE(file.Append(Encode("SET", {"key", "value"})));
  {
    std::ofstream manifest(file.ManifestPath(),
                           std::ios::binary | std::ios::app);
    manifest << "file appendonly.aof.2.incr.aof seq 2 type i\n";
  }
  auto restored = db::RedisDb::Create();
  EXPECT_EQ(Aof::Open(Always(file), restored.get()), nullptr);
}

TEST(AofTest, SkipsAnnotationsAndWritesTimestamps) {
  TempAof file;
  std::string commands = "#TS:1700000000\r\n";
  commands.append(Encode("SET", {"key", "value"}));
  commands.append("#any annotation\r\n");
  ASSERT_TRUE(file.Append(commands));
  auto restored = db::RedisDb::Create();
  Options options = Always(file);
  options.timestamp_enabled = true;
  auto writer = Aof::Open(options, restored.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);

  // aof-timestamp-enabled: an annotation precedes the first command of each
  // second.
  ASSERT_EQ(restored->SetKey("b", db::RedisObject::CreateWithString("1"), 0),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"b", "1"}, restored.get()));
  ASSERT_TRUE(writer->Flush());
  writer.reset();
  const std::string contents = file.Contents();
  const size_t timestamp = contents.rfind("#TS:");
  ASSERT_NE(timestamp, std::string::npos);
  EXPECT_GT(timestamp, commands.size() - 1);
  auto reloaded = db::RedisDb::Create();
  ASSERT_NE(Aof::Open(Always(file), reloaded.get()), nullptr);
  EXPECT_EQ(reloaded->KeyCount(), 2);
}

TEST(AofTest, LoadsRedisSelectRecordsForTheOnlyDatabase) {
  {
    TempAof file;
    std::string commands(kSelect);
    commands.append(Encode("SET", {"key", "value"}));
    ASSERT_TRUE(file.Append(commands));
    auto restored = db::RedisDb::Create();
    ASSERT_NE(Aof::Open(Always(file), restored.get()), nullptr);
    EXPECT_NE(restored->LookupKey("key"), nullptr);
  }
  TempAof file;
  ASSERT_TRUE(file.Append(Encode("SELECT", {"1"}) + Encode("SET", {"k", "v"})));
  auto restored = db::RedisDb::Create();
  EXPECT_EQ(Aof::Open(Always(file), restored.get()), nullptr);
}

TEST(AofTest, RefusesATornTailWithoutAofLoadTruncated) {
  TempAof file;
  ASSERT_TRUE(file.Append(Encode("SET", {"key", "value"})));
  ASSERT_TRUE(file.Append("*2\r\n$3\r\nDEL"));
  Options options = Always(file);
  options.load_truncated = false;
  auto restored = db::RedisDb::Create();
  EXPECT_EQ(Aof::Open(options, restored.get()), nullptr);
}

TEST(AofTest, CutsAPartialWriteOffAndRetriesTheWholeBuffer) {
  // Redis' flushAppendOnlyFile: a short write is truncated back, so a crash
  // leaves no torn record, and the buffer is retried whole.
  TempAof file;
  auto file_ops = std::make_shared<TestFileOps>();
  auto source = db::RedisDb::Create();
  Options options = file.MakeOptions(FsyncPolicy::kNo);
  options.file_ops = file_ops;
  auto writer = Aof::Open(options, source.get());
  ASSERT_NE(writer, nullptr);
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  const size_t record_size =
      kSelect.size() + Encode("SET", {"key", "value"}).size();

  file_ops->FailWritesAfter(5);
  EXPECT_TRUE(writer->Flush());
  EXPECT_FALSE(writer->AcceptingWrites());
  EXPECT_EQ(file.Size(), 0);
  EXPECT_EQ(writer->State().buffer_length, record_size);

  file_ops->RestoreWrites();
  EXPECT_TRUE(writer->Flush());
  EXPECT_TRUE(writer->AcceptingWrites());
  EXPECT_EQ(file.Size(), static_cast<off_t>(record_size));
}

TEST(AofTest, WaitsForARunningSyncBeforeWriting) {
  // Redis' `everysec`: a write waits, up to two seconds, while a background
  // sync of the file runs; a forced flush does not wait.
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
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  ASSERT_TRUE(writer->Flush());
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  file_ops->SlowSyncs(std::chrono::milliseconds(500));
  // Queues a sync, which takes half a second.
  ASSERT_TRUE(writer->Flush());
  ASSERT_EQ(writer->State().pending_fsyncs, 1);

  const off_t size = file.Size();
  ASSERT_TRUE(writer->Append("SET", {"key", "value"}, source.get()));
  ASSERT_TRUE(writer->Flush());
  EXPECT_EQ(file.Size(), size);
  EXPECT_GT(writer->State().buffer_length, 0);
  ASSERT_TRUE(writer->Flush(true));
  EXPECT_GT(file.Size(), size);
  EXPECT_EQ(writer->State().buffer_length, 0);
  file_ops->SlowSyncs(std::chrono::milliseconds(0));
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
  EXPECT_TRUE(writer->State().last_rewrite_ok);
  EXPECT_EQ(writer->State().consecutive_rewrite_failures, 0);
}

TEST(AofTest, CutsOffASmallCorruptTailWhenAllowed) {
  // Redis' aof-load-corrupt-tail-max-size: fewer bytes than the limit follow
  // the last complete command.
  const std::string garbage = "not-resp-at-all";
  for (const size_t limit : {size_t{0}, garbage.size(), garbage.size() + 1}) {
    TempAof file;
    ASSERT_TRUE(file.Append(Encode("SET", {"key", "value"})));
    ASSERT_TRUE(file.Append(garbage));
    Options options = Always(file);
    options.load_corrupt_tail_max_bytes = limit;
    auto restored = db::RedisDb::Create();
    auto reader = Aof::Open(options, restored.get());
    if (limit <= garbage.size()) {
      // Off (0), or too small for the damage: refuse to load.
      EXPECT_EQ(reader, nullptr) << limit;
      continue;
    }
    ASSERT_NE(reader, nullptr);
    ASSERT_NE(restored->LookupKey("key"), nullptr);
    reader.reset();
    EXPECT_EQ(file.Size(),
              static_cast<off_t>(Encode("SET", {"key", "value"}).size()));
  }
}

TEST(AofTest, NeverCutsOffCorruptionInAnEarlierFile) {
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
  }
  const std::vector<std::string> files = file.Files();
  ASSERT_EQ(files.size(), 2);
  {
    std::ofstream base(files.front(), std::ios::binary | std::ios::app);
    base << "garbage";
  }
  Options options = Always(file);
  options.load_corrupt_tail_max_bytes = 1024;
  auto restored = db::RedisDb::Create();
  EXPECT_EQ(Aof::Open(options, restored.get()), nullptr);
}
TEST(AofTest, RewritesTheBaseAsAnRdbFile) {
  TempAof file;
  auto source = db::RedisDb::Create();
  auto writer = Aof::Open(WithRdbPreamble(file), source.get());
  ASSERT_NE(writer, nullptr);
  // The base created at the first start is an RDB file too, as in Redis.
  EXPECT_EQ(file.ManifestText(),
            "file appendonly.aof.1.base.rdb seq 1 type b\n"
            "file appendonly.aof.1.incr.aof seq 1 type i\n");
  const int64_t expiration = utils::NowInMilliseconds() + int64_t{3600} * 1000;
  ASSERT_EQ(source->SetKey("text", db::RedisObject::CreateWithString("value"),
                           expiration),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"text", "value"}, source.get()));
  ASSERT_EQ(writer->StartRewrite(source.get()), RewriteResult::kStarted);
  writer->WaitForRewrite();
  ASSERT_TRUE(writer->State().last_rewrite_ok);
  // Written after the rewrite started, so replayed after the RDB base.
  ASSERT_EQ(source->SetKey("after", db::RedisObject::CreateWithString("1"), 0),
            db::DbStatus::kOk);
  ASSERT_TRUE(writer->Append("SET", {"after", "1"}, source.get()));
  writer.reset();

  const std::string base = file.Files().front();
  EXPECT_TRUE(base.ends_with("/appendonly.aof.2.base.rdb")) << base;
  EXPECT_TRUE(file.Contents().starts_with("REDIS0011"));
  auto restored = db::RedisDb::Create();
  ASSERT_NE(Aof::Open(WithRdbPreamble(file), restored.get()), nullptr);
  EXPECT_EQ(restored->KeyCount(), 2);
  EXPECT_EQ(restored->LookupKey("text")->String(), "value");
  EXPECT_EQ(restored->Expiration("text"), expiration);

  // Either format loads whatever the option says; the next rewrite writes
  // the configured one.
  restored = db::RedisDb::Create();
  auto commands = Aof::Open(Always(file), restored.get());
  ASSERT_NE(commands, nullptr);
  EXPECT_EQ(restored->KeyCount(), 2);
  ASSERT_EQ(commands->StartRewrite(restored.get()), RewriteResult::kStarted);
  commands->WaitForRewrite();
  ASSERT_TRUE(commands->State().last_rewrite_ok);
  EXPECT_EQ(commands->CurrentManifest().base.value_or(ManifestFile{}).name,
            "appendonly.aof.3.base.aof");
}

TEST(AofTest, LoadsAnRdbPreambleAndTheCommandsAfterIt) {
  // An old single file Redis wrote with aof-use-rdb-preamble.
  TempAof file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("snapshot", db::RedisObject::CreateWithString("1"), 0),
      db::DbStatus::kOk);
  const std::string preamble = RdbBytes(source.get(), file);
  ASSERT_FALSE(preamble.empty());
  auto restored = db::RedisDb::Create();
  ASSERT_NE(OpenSingleFile(file, preamble + Encode("SET", {"tail", "2"}),
                           restored.get()),
            nullptr);
  EXPECT_EQ(restored->KeyCount(), 2);
  EXPECT_EQ(restored->LookupKey("snapshot")->String(), "1");
  EXPECT_EQ(restored->LookupKey("tail")->String(), "2");
}

TEST(AofTest, KeepsKeysThatExpiredInTheRdbBaseWhileLoading) {
  // Redis loads them, since the commands after the base were written while
  // they existed; here PERSIST saves the key.
  TempAof file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(source->SetKey("key", db::RedisObject::CreateWithString("v"),
                           utils::NowInMilliseconds() + 50),
            db::DbStatus::kOk);
  const std::string preamble = RdbBytes(source.get(), file);
  ASSERT_FALSE(preamble.empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto restored = db::RedisDb::Create();
  ASSERT_NE(OpenSingleFile(file, preamble + Encode("PERSIST", {"key"}),
                           restored.get()),
            nullptr);
  ASSERT_NE(restored->LookupKey("key"), nullptr);
  EXPECT_EQ(restored->Expiration("key"), std::nullopt);
}

TEST(AofTest, CutsATornCommandAfterAnRdbPreamble) {
  // The valid prefix counts from the start of the file, preamble included.
  TempAof file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("snapshot", db::RedisObject::CreateWithString("1"), 0),
      db::DbStatus::kOk);
  const std::string valid =
      RdbBytes(source.get(), file) + Encode("SET", {"tail", "2"});
  auto restored = db::RedisDb::Create();
  ASSERT_NE(OpenSingleFile(file, valid + "*3\r\n$3\r\nSET\r\n$4\r\nto",
                           restored.get()),
            nullptr);
  EXPECT_EQ(restored->KeyCount(), 2);
  EXPECT_EQ(std::filesystem::file_size(file.Directory() + "/appendonly.aof"),
            valid.size());
}

TEST(AofTest, RefusesADamagedRdbBase) {
  TempAof file;
  auto source = db::RedisDb::Create();
  ASSERT_EQ(
      source->SetKey("key", db::RedisObject::CreateWithString("value"), 0),
      db::DbStatus::kOk);
  std::string preamble = RdbBytes(source.get(), file);
  ASSERT_GT(preamble.size(), 20);
  // A changed byte fails the checksum.
  preamble[preamble.size() - 12] ^= 1;
  auto restored = db::RedisDb::Create();
  EXPECT_EQ(OpenSingleFile(file, preamble, restored.get()), nullptr);
}
}  // namespace redis_simple::aof
