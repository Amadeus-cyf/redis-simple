#include "server/aof.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "logging/logger.h"
#include "memory/dynamic_buffer.h"
#include "server/client.h"
#include "server/commands/command.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "server/request_parser.h"
#include "utils/float_utils.h"

namespace redis_simple::aof {
namespace {
constexpr size_t kReadChunkSize = size_t{64} * 1024;
constexpr size_t kSnapshotBatchSize = 64;
// Covers a bulk-string frame or array header, including a size_t decimal value.
constexpr size_t kRespElementOverhead = 32;
constexpr std::string_view kLongestInt64 = "-9223372036854775808";
constexpr auto kSyncInterval = std::chrono::seconds(1);
constexpr auto kRewriteRetryDelay = std::chrono::seconds(5);

class SystemFileOps final : public FileOps {
 public:
  ssize_t Write(int fd, const void* data, size_t size) override {
    return write(fd, data, size);
  }

  ssize_t WriteVector(int fd, const iovec* blocks, int count) override {
    return writev(fd, blocks, count);
  }

  int Sync(int fd) override { return fsync(fd); }

  int Rename(const char* source, const char* destination) override {
    return std::rename(source, destination);
  }
};

std::shared_ptr<FileOps> DefaultFileOps() {
  static auto file_ops = std::make_shared<SystemFileOps>();
  return file_ops;
}

size_t MaxIovCount() {
  static const size_t max_iov_count = [] {
    const long system_limit = sysconf(_SC_IOV_MAX);
    const size_t limit =
        system_limit > 0 ? static_cast<size_t>(system_limit) : 1024;
    return std::min(limit,
                    static_cast<size_t>(std::numeric_limits<int>::max()));
  }();
  return max_iov_count;
}

bool WriteAll(FileOps* const file_ops, int fd, std::string_view data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t result =
        file_ops->Write(fd, data.data() + written, data.size() - written);
    if (result > 0) {
      written += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool SyncFile(FileOps* const file_ops, int fd) {
  while (file_ops->Sync(fd) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

size_t RespBulkSize(std::string_view value) {
  constexpr size_t kFramingBytes = 5;
  size_t digits = 1;
  for (size_t size = value.size(); size >= 10; size /= 10) {
    ++digits;
  }
  if (value.size() >
      std::numeric_limits<size_t>::max() - digits - kFramingBytes) {
    return std::numeric_limits<size_t>::max();
  }
  return value.size() + digits + kFramingBytes;
}

size_t RespArrayHeaderSize(size_t element_count) {
  constexpr size_t kFramingBytes = 3;
  size_t digits = 1;
  for (size_t size = element_count; size >= 10; size /= 10) {
    ++digits;
  }
  return digits + kFramingBytes;
}

bool AddSize(size_t value, size_t* const total) {
  if (value > std::numeric_limits<size_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

bool ReserveResp(size_t payload_size, size_t element_count,
                 size_t command_count, std::string* const output) {
  const size_t max_size = std::numeric_limits<size_t>::max();
  if (command_count > max_size - element_count) {
    return false;
  }
  const size_t overhead_count = element_count + command_count;
  if (overhead_count > max_size / kRespElementOverhead) {
    return false;
  }
  const size_t overhead = kRespElementOverhead * overhead_count;
  size_t reserve_size = output->size();
  if (!AddSize(payload_size, &reserve_size) ||
      !AddSize(overhead, &reserve_size) || reserve_size > output->max_size()) {
    return false;
  }
  if (reserve_size > output->capacity()) {
    const size_t capacity = output->capacity();
    const size_t grown_capacity =
        capacity <= output->max_size() / 2 ? capacity * 2 : output->max_size();
    output->reserve(std::max(reserve_size, grown_capacity));
  }
  return true;
}

bool ReserveCommands(std::initializer_list<std::string_view> elements,
                     size_t command_count, std::string* const output) {
  size_t payload_size = 0;
  for (const auto element : elements) {
    if (!AddSize(element.size(), &payload_size)) {
      return false;
    }
  }
  return ReserveResp(payload_size, elements.size(), command_count, output);
}

bool ReserveCommand(std::string_view command,
                    const std::vector<std::string_view>& args,
                    std::string* const output) {
  size_t payload_size = command.size();
  for (const auto arg : args) {
    if (!AddSize(arg.size(), &payload_size)) {
      return false;
    }
  }

  if (args.size() == std::numeric_limits<size_t>::max()) {
    return false;
  }
  return ReserveResp(payload_size, args.size() + 1, 1, output);
}

bool ReserveSetRecord(std::string_view key, std::string_view value,
                      bool has_expiration, std::string* const output) {
  if (!has_expiration) {
    return ReserveCommands({"SET", key, value}, 1, output);
  }
  return ReserveCommands({"SET", key, value, "PEXPIREAT", key, kLongestInt64},
                         2, output);
}

bool AppendCommand(std::string_view command,
                   const std::vector<std::string_view>& args,
                   std::string* const output) {
  if (!ReserveCommand(command, args, output)) {
    return false;
  }
  reply::AppendArrayHeader(args.size() + 1, output);
  reply::AppendBulkString(command, output);
  for (std::string_view arg : args) {
    reply::AppendBulkString(arg, output);
  }
  return true;
}

bool AppendFixedCommand(std::initializer_list<std::string_view> elements,
                        std::string* const output) {
  if (elements.size() == 0 || !ReserveCommands(elements, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(elements.size(), output);
  for (const auto element : elements) {
    reply::AppendBulkString(element, output);
  }
  return true;
}

bool AppendSingleKeyCommand(std::string_view command, std::string_view key,
                            std::string* const output) {
  if (!ReserveCommands({command, key}, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(2, output);
  reply::AppendBulkString(command, output);
  reply::AppendBulkString(key, output);
  return true;
}

bool AppendExpireAt(std::string_view key, int64_t expire, std::string* output) {
  std::array<char, std::numeric_limits<int64_t>::digits10 + 3> buffer{};
  const auto encoded =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), expire);
  if (encoded.ec != std::errc()) {
    return false;
  }
  const std::string_view encoded_expire(
      buffer.data(), static_cast<size_t>(encoded.ptr - buffer.data()));
  if (!ReserveCommands({"PEXPIREAT", key, encoded_expire}, 1, output)) {
    return false;
  }
  reply::AppendArrayHeader(3, output);
  reply::AppendBulkString("PEXPIREAT", output);
  reply::AppendBulkString(key, output);
  reply::AppendBulkString(encoded_expire, output);
  return true;
}

template <typename Sink>
class SnapshotBatch {
 public:
  SnapshotBatch(std::string_view command, std::string_view key,
                size_t elements_per_item, const Limits& limits, Sink* sink)
      : command_(command),
        key_(key),
        elements_per_item_(elements_per_item),
        limits_(&limits),
        sink_(sink) {}

  bool Append(std::initializer_list<std::string_view> elements) {
    if (elements.size() != elements_per_item_) {
      return false;
    }
    size_t item_size = 0;
    for (const auto element : elements) {
      const size_t encoded_size = RespBulkSize(element);
      if (encoded_size == std::numeric_limits<size_t>::max() ||
          !AddSize(encoded_size, &item_size)) {
        return false;
      }
    }

    if (item_count_ > 0 &&
        (item_count_ == kSnapshotBatchSize ||
         body_.size() >
             limits_->snapshot_block_bytes -
                 std::min(item_size, limits_->snapshot_block_bytes))) {
      if (!Flush()) {
        return false;
      }
    }

    for (const auto element : elements) {
      reply::AppendBulkString(element, &body_);
    }
    ++item_count_;
    return CommandSize() <= limits_->max_replay_command_bytes;
  }

  bool Finish() { return Flush(); }

 private:
  [[nodiscard]] size_t CommandSize() const {
    const size_t element_count = 2 + (item_count_ * elements_per_item_);
    size_t size = RespArrayHeaderSize(element_count);
    if (!AddSize(RespBulkSize(command_), &size) ||
        !AddSize(RespBulkSize(key_), &size) || !AddSize(body_.size(), &size)) {
      return std::numeric_limits<size_t>::max();
    }
    return size;
  }

  bool Flush() {
    if (item_count_ == 0) {
      return true;
    }
    const size_t element_count = 2 + (item_count_ * elements_per_item_);
    std::string header = reply::FromArrayHeader(element_count);
    reply::AppendBulkString(command_, &header);
    reply::AppendBulkString(key_, &header);
    if (body_.size() > limits_->max_replay_command_bytes ||
        header.size() > limits_->max_replay_command_bytes - body_.size() ||
        !(*sink_)(std::move(header)) || !(*sink_)(std::move(body_))) {
      return false;
    }
    body_.clear();
    item_count_ = 0;
    return true;
  }

  std::string_view command_;
  std::string_view key_;
  size_t elements_per_item_;
  const Limits* limits_;
  Sink* sink_;
  std::string body_;
  size_t item_count_{};
};

template <typename Sink>
bool EmitFixedCommand(std::initializer_list<std::string_view> elements,
                      const Limits& limits, Sink* sink) {
  std::string record;
  if (!AppendFixedCommand(elements, &record) ||
      record.size() > limits.max_replay_command_bytes) {
    return false;
  }
  return (*sink)(std::move(record));
}

template <typename Sink>
bool EmitString(std::string_view key, std::string_view value,
                const Limits& limits, Sink* sink) {
  size_t fixed_overhead = RespBulkSize("APPEND");
  if (!AddSize(RespBulkSize(key), &fixed_overhead) ||
      !AddSize(64, &fixed_overhead)) {
    return false;
  }
  if (fixed_overhead >= limits.max_replay_command_bytes) {
    return false;
  }
  const size_t target =
      std::min(limits.snapshot_block_bytes, limits.max_replay_command_bytes);
  const size_t chunk_size = std::max<size_t>(
      1, target > fixed_overhead
             ? target - fixed_overhead
             : limits.max_replay_command_bytes - fixed_overhead);
  if (value.size() <= chunk_size) {
    return EmitFixedCommand({"SET", key, value}, limits, sink);
  }
  if (!EmitFixedCommand({"SET", key, ""}, limits, sink)) {
    return false;
  }
  for (size_t offset = 0; offset < value.size(); offset += chunk_size) {
    const size_t size = std::min(chunk_size, value.size() - offset);
    if (!EmitFixedCommand({"APPEND", key, value.substr(offset, size)}, limits,
                          sink)) {
      return false;
    }
  }
  return true;
}

template <typename Sink>
bool AppendSnapshotRecord(std::string_view key, const db::RedisObject& object,
                          std::optional<int64_t> expire, const Limits& limits,
                          Sink* sink) {
  bool encoded = false;
  switch (object.Type()) {
    case db::RedisObject::ObjectType::kString:
      encoded = EmitString(key, object.String(), limits, sink);
      break;
    case db::RedisObject::ObjectType::kSet: {
      const auto* set = object.Set();
      SnapshotBatch batch("SADD", key, 1, limits, sink);
      encoded = set->ForEachMember([&batch](std::string_view member) {
        return batch.Append({member});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kList: {
      const auto* list = object.List();
      const size_t size = list->Size();
      if (size == 0) {
        encoded = true;
        break;
      }
      SnapshotBatch batch("RPUSH", key, 1, limits, sink);
      encoded = list->ForEach(0, size - 1, [&batch](std::string_view value) {
        return batch.Append({value});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kZSet: {
      const auto* zset = object.ZSet();
      SnapshotBatch batch("ZADD", key, 2, limits, sink);
      encoded = zset->ForEachEntry([&batch](std::string_view member,
                                            double score) {
        const std::string score_text = utils::FloatToString(score);
        return batch.Append({score_text, member});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kHash: {
      const auto* hash = object.Hash();
      SnapshotBatch batch("HSET", key, 2, limits, sink);
      encoded = hash->ForEachEntry([&batch](std::string_view field,
                                            std::string_view value) {
        return batch.Append({field, value});
      }) && batch.Finish();
      break;
    }
  }
  if (!encoded) {
    return false;
  }
  if (expire.has_value()) {
    std::string record;
    if (!AppendExpireAt(key, *expire, &record) ||
        record.size() > limits.max_replay_command_bytes ||
        !(*sink)(std::move(record))) {
      return false;
    }
  }
  return true;
}

template <typename Sink>
bool BuildSnapshotRecords(db::RedisDb* const db, const Limits& limits,
                          Sink* sink) {
  return db != nullptr && sink != nullptr &&
         db->ForEachObject([db, &limits, sink](std::string_view key,
                                               const db::RedisObject& object) {
           return AppendSnapshotRecord(key, object, db->Expiration(key), limits,
                                       sink);
         });
}
}  // namespace

std::string_view ErrorName(AofError error) {
  switch (error) {
    case AofError::kNone:
      return "none";
    case AofError::kQueueFull:
      return "queue-full";
    case AofError::kWrite:
      return "write";
    case AofError::kSync:
      return "sync";
    case AofError::kRename:
      return "rename";
    case AofError::kDirectorySync:
      return "directory-sync";
    case AofError::kSnapshot:
      return "snapshot";
  }
  return "unknown";
}

std::string_view RewriteStatusName(RewriteStatus status) {
  switch (status) {
    case RewriteStatus::kNeverRun:
      return "none";
    case RewriteStatus::kSucceeded:
      return "ok";
    case RewriteStatus::kFailed:
      return "err";
  }
  return "unknown";
}

char* Aof::Record::Data() {
  if (auto* owned = std::get_if<std::string>(&data_)) {
    return owned->data();
  }
  return (*std::get_if<std::shared_ptr<std::string>>(&data_))->data();
}

size_t Aof::Record::Size() const {
  if (const auto* owned = std::get_if<std::string>(&data_)) {
    return owned->size();
  }
  return (*std::get_if<std::shared_ptr<std::string>>(&data_))->size();
}

std::unique_ptr<Aof> Aof::Open(const Options& options, db::RedisDb* const db) {
  if (db == nullptr || options.path.empty() ||
      options.limits.max_replay_command_bytes == 0 ||
      options.limits.snapshot_block_bytes == 0 ||
      options.limits.max_snapshot_queue_bytes == 0) {
    return nullptr;
  }
  int open_flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(options.path.c_str(), open_flags, 0644);
  if (fd < 0) {
    return nullptr;
  }
  struct stat file_info{};
  if (fstat(fd, &file_info) < 0 || file_info.st_size < 0 ||
      static_cast<uintmax_t>(file_info.st_size) >
          std::numeric_limits<size_t>::max()) {
    close(fd);
    return nullptr;
  }
  auto aof = std::unique_ptr<Aof>(
      new Aof(fd, options, static_cast<size_t>(file_info.st_size)));
  if (!aof->SyncParentDirectory() || !aof->Replay(db) ||
      lseek(fd, 0, SEEK_END) < 0 || fstat(fd, &file_info) < 0 ||
      file_info.st_size < 0 ||
      static_cast<uintmax_t>(file_info.st_size) >
          std::numeric_limits<size_t>::max()) {
    return nullptr;
  }
  aof->current_size_ = static_cast<size_t>(file_info.st_size);
  aof->base_size_ = aof->current_size_;
  if (options.fsync != FsyncPolicy::kAlways) {
    try {
      aof->worker_ = std::thread([instance = aof.get()] { instance->Run(); });
    } catch (const std::system_error&) {
      return nullptr;
    }
  }
  return aof;
}

Aof::Aof(int fd, const Options& options, size_t file_size)
    : fd_(fd),
      path_(options.path),
      fsync_(options.fsync),
      auto_rewrite_min_bytes_(options.auto_rewrite_min_bytes),
      auto_rewrite_percentage_(options.auto_rewrite_percentage),
      limits_(options.limits),
      file_ops_(options.file_ops != nullptr ? options.file_ops
                                            : DefaultFileOps()),
      current_size_(file_size),
      base_size_(file_size),
      last_sync_(std::chrono::steady_clock::now()) {}

bool Aof::Append(std::string_view command,
                 const std::vector<std::string_view>& args,
                 db::RedisDb* const db) {
  const size_t batch_size = batch_.size();
  if (!Encode(command, args, db, &batch_)) {
    // Drop any partially encoded record so the batch stays replayable.
    batch_.resize(batch_size);
    return false;
  }
  return true;
}

bool Aof::Flush() {
  if (batch_.empty()) {
    return Healthy();
  }
  std::string batch;
  batch.swap(batch_);
  return Submit(std::move(batch));
}

bool Aof::Submit(std::string batch) {
  if (fsync_ == FsyncPolicy::kAlways) {
    const std::scoped_lock lock(mutex_);
    if (!healthy_ || stopping_) {
      return false;
    }
    std::shared_ptr<std::string> shared;
    std::string_view data = batch;
    if (rewriting_ && !rewrite_failed_) {
      shared = std::make_shared<std::string>(std::move(batch));
      data = *shared;
      BufferRewriteCommandLocked(shared);
    }
    // One write and one fsync cover every mutation in the batch.
    if (!WriteAll(file_ops_.get(), fd_, data)) {
      FailLocked(AofError::kWrite);
      return false;
    }
    if (!SyncFile(fd_)) {
      FailLocked(AofError::kSync);
      return false;
    }
    if (current_size_ > std::numeric_limits<size_t>::max() - data.size()) {
      FailLocked(AofError::kWrite);
      return false;
    }
    current_size_ += data.size();
    return true;
  }

  {
    std::unique_lock<std::mutex> lock(mutex_);
    // Apply backpressure instead of failing when the writer falls behind. An
    // empty queue always admits one batch, so an oversized batch cannot wait
    // forever.
    space_available_.wait(lock, [this, &batch] {
      return !healthy_ || stopping_ || pending_.empty() ||
             (batch.size() <= limits_.max_pending_bytes &&
              pending_bytes_ <= limits_.max_pending_bytes - batch.size() &&
              pending_.size() < limits_.max_pending_commands);
    });
    if (!healthy_ || stopping_) {
      return false;
    }
    if (current_size_ > std::numeric_limits<size_t>::max() - batch.size()) {
      FailLocked(AofError::kWrite);
      return false;
    }
    pending_bytes_ += batch.size();
    current_size_ += batch.size();
    if (rewriting_ && !rewrite_failed_) {
      auto shared = std::make_shared<std::string>(std::move(batch));
      BufferRewriteCommandLocked(shared);
      pending_.emplace_back(std::move(shared));
    } else {
      pending_.emplace_back(std::move(batch));
    }
  }
  work_available_.notify_one();
  return true;
}

RewriteResult Aof::StartRewrite(db::RedisDb* const db) {
  // Mutations already applied to `db` belong in the current file; the
  // snapshot covers them, so they must not land in the rewrite delta.
  if (!Flush()) {
    return RewriteResult::kError;
  }
  std::thread completed_rewrite;
  {
    const std::scoped_lock lock(mutex_);
    if (!healthy_ || stopping_ || db == nullptr) {
      return RewriteResult::kError;
    }
    if (rewriting_) {
      return RewriteResult::kInProgress;
    }
    if (rewrite_worker_.joinable()) {
      completed_rewrite = std::move(rewrite_worker_);
    }
    rewriting_ = true;
    rewrite_failed_ = false;
    snapshot_complete_ = false;
    snapshot_pending_.clear();
    rewrite_pending_.clear();
    snapshot_pending_bytes_ = 0;
    rewrite_pending_bytes_ = 0;
    last_error_ = AofError::kNone;
    last_rewrite_attempt_ = std::chrono::steady_clock::now();
  }
  if (completed_rewrite.joinable()) {
    completed_rewrite.join();
  }

  std::string path_template = path_ + ".rewrite.XXXXXX";
  std::vector<char> writable_path(path_template.begin(), path_template.end());
  writable_path.push_back('\0');
  const int rewrite_fd = mkstemp(writable_path.data());
  if (rewrite_fd < 0) {
    const std::scoped_lock lock(mutex_);
    FinishRewriteLocked(RewriteStatus::kFailed, AofError::kWrite);
    return RewriteResult::kError;
  }
#ifdef FD_CLOEXEC
  if (fcntl(rewrite_fd, F_SETFD, FD_CLOEXEC) < 0) {
    close(rewrite_fd);
    unlink(writable_path.data());
    const std::scoped_lock lock(mutex_);
    FinishRewriteLocked(RewriteStatus::kFailed, AofError::kWrite);
    return RewriteResult::kError;
  }
#endif

  struct stat file_info{};
  const mode_t mode = fstat(fd_, &file_info) == 0
                          ? static_cast<mode_t>(file_info.st_mode & 0777)
                          : static_cast<mode_t>(0644);
  if (fchmod(rewrite_fd, mode) < 0) {
    close(rewrite_fd);
    unlink(writable_path.data());
    const std::scoped_lock lock(mutex_);
    FinishRewriteLocked(RewriteStatus::kFailed, AofError::kWrite);
    return RewriteResult::kError;
  }

  try {
    rewrite_worker_ = std::thread(
        [this, rewrite_fd, temp_path = std::string(writable_path.data())] {
          RunRewrite(rewrite_fd, temp_path);
        });
  } catch (const std::system_error&) {
    close(rewrite_fd);
    unlink(writable_path.data());
    const std::scoped_lock lock(mutex_);
    FinishRewriteLocked(RewriteStatus::kFailed, AofError::kWrite);
    return RewriteResult::kError;
  }

  const bool snapshot_built = BuildSnapshot(db);
  {
    const std::scoped_lock lock(mutex_);
    if (rewriting_) {
      snapshot_complete_ = true;
      if (!snapshot_built) {
        rewrite_failed_ = true;
        rewrite_status_ = RewriteStatus::kFailed;
        last_error_ = AofError::kSnapshot;
      }
    }
  }
  rewrite_state_changed_.notify_all();
  if (!snapshot_built) {
    WaitUntilRewriteIdle();
    return RewriteResult::kError;
  }
  return RewriteResult::kStarted;
}

bool Aof::ShouldAutoRewrite() const {
  const std::scoped_lock lock(mutex_);
  const size_t current_size = current_size_ + batch_.size();
  if (!healthy_ || stopping_ || rewriting_ || auto_rewrite_percentage_ == 0 ||
      current_size == 0 || current_size < auto_rewrite_min_bytes_) {
    return false;
  }
  if (rewrite_status_ == RewriteStatus::kFailed &&
      std::chrono::steady_clock::now() - last_rewrite_attempt_ <
          kRewriteRetryDelay) {
    return false;
  }
  if (base_size_ == 0) {
    return true;
  }
  const size_t growth = current_size - std::min(current_size, base_size_);
  return static_cast<long double>(growth) * 100.0L >=
         static_cast<long double>(base_size_) *
             static_cast<long double>(auto_rewrite_percentage_);
}

AofState Aof::State() const {
  const std::scoped_lock lock(mutex_);
  return {healthy_,
          rewriting_,
          rewrite_status_,
          last_error_,
          current_size_ + batch_.size(),
          base_size_,
          pending_bytes_ + batch_.size()};
}

bool Aof::Healthy() const {
  const std::scoped_lock lock(mutex_);
  return healthy_;
}

void Aof::WaitUntilIdle() {
  Flush();
  if (fsync_ == FsyncPolicy::kAlways) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] {
    return (!writing_ && !syncing_ && pending_.empty()) || !healthy_;
  });
}

void Aof::WaitUntilRewriteIdle() {
  Flush();
  std::unique_lock<std::mutex> lock(mutex_);
  rewrite_state_changed_.wait(lock, [this] { return !rewriting_; });
}

Aof::~Aof() {
  Flush();
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  work_available_.notify_all();
  space_available_.notify_all();
  rewrite_state_changed_.notify_all();
  if (rewrite_worker_.joinable()) {
    rewrite_worker_.join();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  close(fd_);
}

bool Aof::Replay(db::RedisDb* const db) {
  if (lseek(fd_, 0, SEEK_SET) < 0) {
    return false;
  }

  db->SetLoading(true);
  const bool replayed = [this, db] {
    Client client(db);
    std::array<char, kReadChunkSize> chunk{};
    in_memory::DynamicBuffer input;
    command::CommandArgs args;
    size_t complete_bytes = 0;
    bool reached_end = false;

    while (!reached_end) {
      ssize_t bytes_read = -1;
      while (true) {
        bytes_read = read(fd_, chunk.data(), chunk.size());
        if (bytes_read >= 0 || errno != EINTR) {
          break;
        }
      }
      if (bytes_read < 0) {
        return false;
      }
      reached_end = bytes_read == 0;
      if (bytes_read > 0) {
        input.Append(chunk.data(), static_cast<size_t>(bytes_read));
      }

      while (!input.Empty()) {
        const std::string_view remaining = input.View();
        if (remaining.front() != '*') {
          return false;
        }
        std::string_view name;
        const auto parsed = request_parser::Parse(remaining, &name, &args);
        if (parsed.status == request_parser::ParseStatus::kIncomplete) {
          break;
        }
        if (parsed.status != request_parser::ParseStatus::kComplete) {
          return false;
        }
        if (parsed.consumed > limits_.max_replay_command_bytes) {
          return false;
        }
        const auto* metadata = command::Find(name);
        if (metadata == nullptr ||
            metadata->access != command::CommandAccess::kWrite ||
            !metadata->arity.Accepts(args.size()) ||
            !client.ExecuteForReplay(metadata, &args)) {
          return false;
        }
        if (!AddSize(parsed.consumed, &complete_bytes)) {
          return false;
        }
        input.Consume(parsed.consumed);
      }
      if (input.View().size() > limits_.max_replay_command_bytes) {
        return false;
      }
    }

    if (!input.Empty()) {
      if (complete_bytes >
              static_cast<uintmax_t>(std::numeric_limits<off_t>::max()) ||
          ftruncate(fd_, static_cast<off_t>(complete_bytes)) < 0) {
        return false;
      }
    }
    return input.Empty() || SyncFile(fd_);
  }();
  db->SetLoading(false);
  return replayed;
}

bool Aof::Encode(std::string_view command,
                 const std::vector<std::string_view>& args,
                 db::RedisDb* const db, std::string* const output) {
  if (db == nullptr || output == nullptr) {
    return false;
  }
  if (command == "SET") {
    if (args.empty()) {
      return false;
    }
    const auto* object = db->LookupKey(args[0]);
    if (object == nullptr) {
      return AppendSingleKeyCommand("DEL", args[0], output);
    }
    if (object->Type() != db::RedisObject::ObjectType::kString) {
      return false;
    }
    const auto expire = db->Expiration(args[0]);
    if (!ReserveSetRecord(args[0], object->String(), expire.has_value(),
                          output)) {
      return false;
    }
    reply::AppendArrayHeader(3, output);
    reply::AppendBulkString("SET", output);
    reply::AppendBulkString(args[0], output);
    reply::AppendBulkString(object->String(), output);
    return !expire.has_value() || AppendExpireAt(args[0], *expire, output);
  }
  if (command == "EXPIRE" || command == "EXPIREAT" || command == "PEXPIRE" ||
      command == "PEXPIREAT") {
    if (args.empty()) {
      return false;
    }
    const auto expire = db->Expiration(args[0]);
    if (expire.has_value()) {
      return AppendExpireAt(args[0], *expire, output);
    }
    return AppendSingleKeyCommand("DEL", args[0], output);
  }
  return AppendCommand(command, args, output);
}

bool Aof::BuildSnapshot(db::RedisDb* const db) {
  auto sink = [this](std::string block) {
    return QueueSnapshotBlock(std::move(block));
  };
  return BuildSnapshotRecords(db, limits_, &sink);
}

bool Aof::QueueSnapshotBlock(std::string block) {
  if (block.empty()) {
    return true;
  }
  const size_t block_size = block.size();
  std::unique_lock<std::mutex> lock(mutex_);
  rewrite_state_changed_.wait(lock, [this, block_size] {
    // One oversized block may enter an empty queue so a valid replay command
    // cannot deadlock behind the softer snapshot queue limit.
    return !rewriting_ || rewrite_failed_ || stopping_ ||
           snapshot_pending_.empty() ||
           (block_size <= limits_.max_snapshot_queue_bytes &&
            snapshot_pending_bytes_ <=
                limits_.max_snapshot_queue_bytes - block_size);
  });
  if (!rewriting_ || rewrite_failed_ || stopping_) {
    return false;
  }
  if (snapshot_pending_bytes_ >
      std::numeric_limits<size_t>::max() - block_size) {
    rewrite_failed_ = true;
    last_error_ = AofError::kSnapshot;
    return false;
  }
  snapshot_pending_bytes_ += block_size;
  snapshot_pending_.emplace_back(std::move(block));
  rewrite_state_changed_.notify_all();
  return true;
}

bool Aof::WriteBatch(int fd, RecordQueue& batch,
                     std::vector<iovec>* const blocks) const {
  const size_t max_iov_count = MaxIovCount();
  blocks->reserve(std::min(batch.size(), max_iov_count));
  auto record = batch.begin();

  while (record != batch.end()) {
    blocks->clear();
    for (; record != batch.end() && blocks->size() < max_iov_count; ++record) {
      blocks->push_back({record->Data(), record->Size()});
    }

    size_t index = 0;
    while (index < blocks->size()) {
      ssize_t written = -1;
      while (true) {
        written =
            file_ops_->WriteVector(fd, blocks->data() + index,
                                   static_cast<int>(blocks->size() - index));
        if (written >= 0 || errno != EINTR) {
          break;
        }
      }
      if (written <= 0) {
        return false;
      }
      auto consumed = static_cast<size_t>(written);
      while (consumed > 0) {
        auto& block = (*blocks)[index];
        if (consumed < block.iov_len) {
          auto* data = static_cast<char*>(block.iov_base);
          block.iov_base = data + consumed;
          block.iov_len -= consumed;
          consumed = 0;
        } else {
          consumed -= block.iov_len;
          ++index;
        }
      }
    }
  }
  return true;
}

void Aof::Run() {
  RecordQueue batch;
  std::vector<iovec> blocks;
  while (true) {
    bool should_sync = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      while (healthy_ && pending_.empty() && !stopping_) {
        if (fsync_ == FsyncPolicy::kEverySecond && dirty_) {
          const auto deadline = last_sync_ + kSyncInterval;
          if (!work_available_.wait_until(lock, deadline, [this] {
                return stopping_ || !pending_.empty() || !healthy_;
              })) {
            break;
          }
        } else {
          work_available_.wait(lock, [this] {
            return stopping_ || !pending_.empty() || !healthy_;
          });
        }
      }
      if (!healthy_) {
        return;
      }
      if (!pending_.empty()) {
        batch.swap(pending_);
        pending_bytes_ = 0;
        writing_ = true;
        space_available_.notify_all();
      } else if (dirty_ &&
                 (stopping_ || (fsync_ == FsyncPolicy::kEverySecond &&
                                std::chrono::steady_clock::now() - last_sync_ >=
                                    kSyncInterval))) {
        syncing_ = true;
        should_sync = true;
      } else if (stopping_) {
        return;
      }
    }

    if (!batch.empty()) {
      const bool written = WriteBatch(fd_, batch, &blocks);
      {
        const std::scoped_lock lock(mutex_);
        writing_ = false;
        if (!written) {
          FailLocked(AofError::kWrite);
          return;
        }
        dirty_ = true;
        NotifyIfIdleLocked();
      }
      batch.clear();
      continue;
    }

    if (should_sync) {
      const bool synced = SyncFile(fd_);
      {
        const std::scoped_lock lock(mutex_);
        syncing_ = false;
        if (!synced) {
          FailLocked(AofError::kSync);
          return;
        }
        dirty_ = false;
        last_sync_ = std::chrono::steady_clock::now();
        NotifyIfIdleLocked();
      }
    }
  }
}

void Aof::RunRewrite(int rewrite_fd, const std::string& temp_path) {
  RecordQueue batch;
  std::vector<iovec> blocks;
  bool succeeded = true;
  AofError error = AofError::kNone;

  while (succeeded) {
    std::unique_lock<std::mutex> lock(mutex_);
    rewrite_state_changed_.wait(lock, [this] {
      return !healthy_ || rewrite_failed_ || stopping_ ||
             !snapshot_pending_.empty() || snapshot_complete_;
    });
    if (!healthy_ || rewrite_failed_ || stopping_) {
      error =
          last_error_ == AofError::kNone ? AofError::kSnapshot : last_error_;
      break;
    }
    if (snapshot_pending_.empty()) {
      break;
    }
    batch.swap(snapshot_pending_);
    snapshot_pending_bytes_ = 0;
    rewrite_state_changed_.notify_all();
    lock.unlock();
    succeeded = WriteBatch(rewrite_fd, batch, &blocks);
    if (!succeeded) {
      error = AofError::kWrite;
    }
    batch.clear();
  }

  while (succeeded) {
    std::unique_lock<std::mutex> lock(mutex_);
    rewrite_state_changed_.wait(lock, [this] {
      return !healthy_ || rewrite_failed_ || !rewrite_pending_.empty() ||
             (!writing_ && !syncing_ && pending_.empty());
    });
    if (!healthy_ || rewrite_failed_) {
      error =
          last_error_ == AofError::kNone ? AofError::kSnapshot : last_error_;
      break;
    }
    if (!rewrite_pending_.empty()) {
      batch.swap(rewrite_pending_);
      rewrite_pending_bytes_ = 0;
      lock.unlock();
      succeeded = WriteBatch(rewrite_fd, batch, &blocks);
      if (!succeeded) {
        error = AofError::kWrite;
      }
      batch.clear();
      continue;
    }

    struct stat file_info{};
    if (fstat(rewrite_fd, &file_info) < 0 || file_info.st_size < 0) {
      succeeded = false;
      error = AofError::kWrite;
    } else if (!SyncFile(rewrite_fd)) {
      succeeded = false;
      error = AofError::kSync;
    } else if (file_ops_->Rename(temp_path.c_str(), path_.c_str()) < 0) {
      succeeded = false;
      error = AofError::kRename;
    } else {
      const int old_fd = fd_;
      fd_ = rewrite_fd;
      rewrite_fd = -1;
      dirty_ = false;
      last_sync_ = std::chrono::steady_clock::now();
      close(old_fd);
      current_size_ = static_cast<size_t>(file_info.st_size);
      base_size_ = current_size_;
      if (!SyncParentDirectory()) {
        succeeded = false;
        error = AofError::kDirectorySync;
        FailLocked(error);
      }
    }
    FinishRewriteLocked(
        succeeded ? RewriteStatus::kSucceeded : RewriteStatus::kFailed, error);
    work_available_.notify_all();
    lock.unlock();
    if (rewrite_fd >= 0) {
      close(rewrite_fd);
      unlink(temp_path.c_str());
    }
    if (!succeeded) {
      RS_LOG_ERROR("AOF rewrite failed: %.*s\n",
                   static_cast<int>(ErrorName(error).size()),
                   ErrorName(error).data());
    }
    return;
  }

  close(rewrite_fd);
  unlink(temp_path.c_str());
  {
    const std::scoped_lock lock(mutex_);
    FinishRewriteLocked(RewriteStatus::kFailed,
                        error == AofError::kNone ? AofError::kSnapshot : error);
  }
  RS_LOG_ERROR("AOF rewrite failed: %.*s\n",
               static_cast<int>(ErrorName(error).size()),
               ErrorName(error).data());
}

void Aof::BufferRewriteCommandLocked(
    const std::shared_ptr<std::string>& command) {
  if (!rewriting_ || rewrite_failed_) {
    return;
  }
  if (command->size() > limits_.max_pending_bytes ||
      rewrite_pending_bytes_ > limits_.max_pending_bytes - command->size() ||
      rewrite_pending_.size() >= limits_.max_pending_commands) {
    rewrite_failed_ = true;
    rewrite_status_ = RewriteStatus::kFailed;
    last_error_ = AofError::kQueueFull;
    rewrite_pending_.clear();
    rewrite_pending_bytes_ = 0;
  } else {
    rewrite_pending_bytes_ += command->size();
    rewrite_pending_.emplace_back(command);
  }
  rewrite_state_changed_.notify_all();
}

void Aof::FinishRewriteLocked(RewriteStatus status, AofError error) {
  snapshot_pending_.clear();
  rewrite_pending_.clear();
  snapshot_pending_bytes_ = 0;
  rewrite_pending_bytes_ = 0;
  snapshot_complete_ = false;
  rewriting_ = false;
  rewrite_failed_ = status == RewriteStatus::kFailed;
  rewrite_status_ = status;
  if (error != AofError::kNone) {
    last_error_ = error;
  }
  rewrite_state_changed_.notify_all();
}

bool Aof::SyncFile(int fd) const { return aof::SyncFile(file_ops_.get(), fd); }

bool Aof::SyncParentDirectory() const {
  std::filesystem::path parent = std::filesystem::path(path_).parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
  open_flags |= O_DIRECTORY;
#endif
  const int directory_fd = open(parent.c_str(), open_flags);
  if (directory_fd < 0) {
    return false;
  }
  const bool synced = SyncFile(directory_fd);
  const bool closed = close(directory_fd) == 0;
  return synced && closed;
}

void Aof::FailLocked(AofError error) {
  healthy_ = false;
  last_error_ = error;
  pending_.clear();
  pending_bytes_ = 0;
  writing_ = false;
  syncing_ = false;
  dirty_ = false;
  idle_.notify_all();
  space_available_.notify_all();
  rewrite_state_changed_.notify_all();
  RS_LOG_ERROR("AOF became unhealthy: %.*s\n",
               static_cast<int>(ErrorName(error).size()),
               ErrorName(error).data());
}

void Aof::NotifyIfIdleLocked() {
  rewrite_state_changed_.notify_all();
  if (!writing_ && !syncing_ && pending_.empty()) {
    idle_.notify_all();
  }
}
}  // namespace redis_simple::aof
