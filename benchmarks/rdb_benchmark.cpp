#include <benchmark/benchmark.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include "data_types/set/set.h"
#include "server/aof/file_ops.h"
#include "server/db/db.h"
#include "server/db/redis_object.h"
#include "server/rdb/rdb_loader.h"
#include "server/rdb/rdb_writer.h"

// RDB benchmarks: loading and saving a database of sets of integers, which
// an RDB file stores as intsets.
namespace redis_simple {
namespace {
// The integers a database holds, whatever the size of its sets.
constexpr int64_t kIntegerCount = 65'536;

// A directory for a benchmark's file, removed with the file.
class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("rdb_benchmark_" + std::to_string(getpid()))) {
    std::filesystem::create_directories(path_);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  TemporaryDirectory(TemporaryDirectory&&) = delete;
  TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::string FilePath() const {
    return (path_ / "dump.rdb").string();
  }

 private:
  std::filesystem::path path_;
};

// File operations that drop what is written, so that a save is timed
// without the file system.
class DiscardingFileOps final : public aof::FileOps {
 public:
  ssize_t Write(int /*fd*/, const void* /*data*/, size_t size) override {
    return static_cast<ssize_t>(size);
  }
  ssize_t WriteVector(int /*fd*/, const iovec* blocks, int count) override {
    ssize_t size = 0;
    for (const iovec& block : std::span(blocks, static_cast<size_t>(count))) {
      size += static_cast<ssize_t>(block.iov_len);
    }
    return size;
  }
  int Sync(int /*fd*/) override { return 0; }
  int Rename(const char* /*source*/, const char* /*destination*/) override {
    return 0;
  }
};

// A database of kIntegerCount integers in sets of `set_size`. The integers
// of a set take four bytes each.
std::unique_ptr<db::RedisDb> IntegerSets(int64_t set_size) {
  std::unique_ptr<db::RedisDb> db = db::RedisDb::Create();
  const int64_t set_count = kIntegerCount / set_size;
  for (int64_t index = 0; index < set_count; ++index) {
    std::unique_ptr<data_types::Set> set = data_types::Set::Create();
    for (int64_t i = 0; i < set_size; ++i) {
      set->Add(std::to_string((i * 100'000) + index));
    }
    db->SetKey("set:" + std::to_string(index),
               db::RedisObject::CreateWithSet(std::move(set)), 0);
  }
  return db;
}

// Loads a file of sets of range(0) integers: 4,096 sets of 16, or 128 sets
// of 512, the most an intset holds. An iteration creates and destroys the
// database it loads into, which is part of its time.
void RdbLoadIntsets(benchmark::State& state) {
  const TemporaryDirectory directory;
  const std::string path = directory.FilePath();
  const std::unique_ptr<db::RedisDb> source = IntegerSets(state.range(0));
  if (rdb::WriteRdbFile(source.get(), path, rdb::WriteOptions{},
                        aof::DefaultFileOps().get()) !=
      rdb::WriteStatus::kWritten) {
    state.SkipWithError("the file was not written");
    return;
  }
  const rdb::LoadOptions options;
  for (auto _ : state) {
    (void)_;
    const std::unique_ptr<db::RedisDb> db = db::RedisDb::Create();
    const int fd = aof::OpenFile(path, O_RDONLY);
    rdb::LoadStats stats;
    const bool loaded = fd >= 0 && rdb::LoadRdb(fd, db.get(), options, &stats);
    if (fd >= 0) {
      close(fd);
    }
    if (!loaded) {
      state.SkipWithError("the file was not loaded");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations() * kIntegerCount);
}

// Saves the database RdbLoadIntsets loads. The bytes are encoded,
// compressed and summed as for a file, and then dropped.
void RdbSaveIntsets(benchmark::State& state) {
  const TemporaryDirectory directory;
  const std::string path = directory.FilePath();
  const std::unique_ptr<db::RedisDb> db = IntegerSets(state.range(0));
  DiscardingFileOps file_ops;
  const rdb::WriteOptions options;
  for (auto _ : state) {
    (void)_;
    if (rdb::WriteRdbFile(db.get(), path, options, &file_ops) !=
        rdb::WriteStatus::kWritten) {
      state.SkipWithError("the database was not saved");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations() * kIntegerCount);
}
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
BENCHMARK(RdbLoadIntsets)->Arg(16)->Arg(512)->Unit(benchmark::kMicrosecond);
BENCHMARK(RdbSaveIntsets)->Arg(16)->Arg(512)->Unit(benchmark::kMicrosecond);
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables,bugprone-throwing-static-initialization)
}  // namespace redis_simple
