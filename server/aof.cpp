#include "server/aof.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
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
constexpr const std::chrono::seconds kSyncInterval = std::chrono::seconds(1);
constexpr std::string_view kMultiRecord = "*1\r\n$5\r\nMULTI\r\n";

// The full 128-bit product of two 64-bit values, as {high, low} words.
std::pair<uint64_t, uint64_t> WideMultiply(uint64_t left, uint64_t right) {
  constexpr uint64_t kLowHalf = 0xffffffff;
  const uint64_t low_low = (left & kLowHalf) * (right & kLowHalf);
  const uint64_t low_high = (left & kLowHalf) * (right >> 32);
  const uint64_t high_low = (left >> 32) * (right & kLowHalf);
  const uint64_t high_high = (left >> 32) * (right >> 32);
  const uint64_t middle =
      (low_low >> 32) + (low_high & kLowHalf) + (high_low & kLowHalf);
  return {high_high + (low_high >> 32) + (high_low >> 32) + (middle >> 32),
          (middle << 32) | (low_low & kLowHalf)};
}
constexpr std::string_view kExecRecord = "*1\r\n$4\r\nEXEC\r\n";
constexpr const std::chrono::seconds kRewriteRetryDelay =
    std::chrono::seconds(5);

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
  // SystemFileOps is stateless, so each Aof can own its own instance.
  return std::make_shared<SystemFileOps>();
}

size_t MaxIovCount() {
  static const size_t kMaxIovCount = [] {
    const int64_t system_limit = sysconf(_SC_IOV_MAX);
    const size_t limit =
        system_limit > 0 ? static_cast<size_t>(system_limit) : 1024;
    return std::min(limit,
                    static_cast<size_t>(std::numeric_limits<int>::max()));
  }();
  return kMaxIovCount;
}

bool SyncFile(FileOps* const file_ops, int fd) {
  while (file_ops->Sync(fd) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

// Exit codes of the rewrite child, which cannot log.
constexpr int kChildSucceeded = 0;
constexpr int kChildWriteFailed = 1;
constexpr int kChildSyncFailed = 2;
constexpr int kChildSnapshotFailed = 3;

std::optional<size_t> FileSize(int fd) {
  struct stat file_info {};
  if (fstat(fd, &file_info) < 0 || file_info.st_size < 0 ||
      static_cast<uintmax_t>(file_info.st_size) >
          std::numeric_limits<size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<size_t>(file_info.st_size);
}

// Writes as much of `data` as the file takes, retrying short and interrupted
// writes. Returns the bytes written; on an error, sets `error_number`.
size_t WritePrefix(FileOps* const file_ops, int fd, std::string_view data,
                   int* const error_number) {
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
    *error_number = result < 0 ? errno : EIO;
    break;
  }
  return written;
}

// Writes every block with vectored writes, retrying short and interrupted
// writes.
template <typename Blocks>
bool WriteBlocks(FileOps* const file_ops, int fd, Blocks& blocks) {
  const size_t max_iov_count = MaxIovCount();
  std::vector<iovec> vectors;
  auto block = std::begin(blocks);
  while (block != std::end(blocks)) {
    vectors.clear();
    for (; block != std::end(blocks) && vectors.size() < max_iov_count;
         ++block) {
      if (!block->empty()) {
        vectors.push_back({block->data(), block->size()});
      }
    }
    size_t index = 0;
    while (index < vectors.size()) {
      ssize_t written = -1;
      while (true) {
        written =
            file_ops->WriteVector(fd, vectors.data() + index,
                                  static_cast<int>(vectors.size() - index));
        if (written >= 0 || errno != EINTR) {
          break;
        }
      }
      if (written <= 0) {
        return false;
      }
      auto consumed = static_cast<size_t>(written);
      while (consumed > 0) {
        iovec& vector = vectors[index];
        if (consumed < vector.iov_len) {
          vector.iov_base = static_cast<char*>(vector.iov_base) + consumed;
          vector.iov_len -= consumed;
          consumed = 0;
        } else {
          consumed -= vector.iov_len;
          ++index;
        }
      }
    }
  }
  return true;
}

// waitpid, retried when interrupted. Returns the child's pid, 0 if it is
// still running (with WNOHANG), or -1.
pid_t WaitForChild(pid_t child, int* const wait_status, int options) {
  while (true) {
    const pid_t result = waitpid(child, wait_status, options);
    if (result >= 0 || errno != EINTR) {
      return result;
    }
  }
}

AofError ChildError(int wait_status) {
  if (!WIFEXITED(wait_status)) {
    return AofError::kSnapshot;
  }
  switch (WEXITSTATUS(wait_status)) {
    case kChildWriteFailed:
      return AofError::kWrite;
    case kChildSyncFailed:
      return AofError::kSync;
    default:
      return AofError::kSnapshot;
  }
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
      const set::Set* set = object.Set();
      SnapshotBatch batch("SADD", key, 1, limits, sink);
      encoded = set->ForEachMember([&batch](std::string_view member) {
        return batch.Append({member});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kList: {
      const list::List* list = object.List();
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
      const zset::ZSet* zset = object.ZSet();
      SnapshotBatch batch("ZADD", key, 2, limits, sink);
      encoded = zset->ForEachEntry([&batch](std::string_view member,
                                            double score) {
        const std::string score_text = utils::FloatToString(score);
        return batch.Append({score_text, member});
      }) && batch.Finish();
      break;
    }
    case db::RedisObject::ObjectType::kHash: {
      const hash::Hash* hash = object.Hash();
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
// Commands of one MULTI/EXEC block read during replay, held until EXEC.
class ReplayTransaction {
 public:
  void Queue(const command::Command* command,
             const command::CommandArgs& args) {
    commands_.push_back({command, args.size()});
    for (const std::string_view arg : args) {
      args_.emplace_back(arg);
    }
  }

  // Runs and clears the queued commands through `execute`, which returns
  // false when a command fails; Apply then stops and returns false.
  template <typename Execute>
  bool Apply(const Execute& execute) {
    command::CommandArgs views;
    size_t next_arg = 0;
    bool applied = true;
    for (const auto& [command, arg_count] : commands_) {
      views.assign(
          args_.begin() + static_cast<std::ptrdiff_t>(next_arg),
          args_.begin() + static_cast<std::ptrdiff_t>(next_arg + arg_count));
      next_arg += arg_count;
      if (!execute(command, &views)) {
        applied = false;
        break;
      }
    }
    commands_.clear();
    args_.clear();
    return applied;
  }

 private:
  struct QueuedCommand {
    const command::Command* command;
    size_t arg_count;
  };
  std::vector<QueuedCommand> commands_;
  std::vector<std::string> args_;
};
}  // namespace

std::string_view ErrorName(AofError error) {
  switch (error) {
    case AofError::kNone:
      return "none";
    case AofError::kRewriteBufferFull:
      return "rewrite-buffer-full";
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

std::unique_ptr<Aof> Aof::Open(const Options& options, db::RedisDb* const db) {
  if (db == nullptr || options.path.empty() ||
      options.limits.max_replay_command_bytes == 0 ||
      options.limits.snapshot_block_bytes == 0) {
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
  const std::optional<size_t> file_size = FileSize(fd);
  if (!file_size.has_value()) {
    close(fd);
    return nullptr;
  }
  auto aof = std::unique_ptr<Aof>(new Aof(fd, options, *file_size));
  if (!aof->SyncParentDirectory() || !aof->Replay(db) ||
      lseek(fd, 0, SEEK_END) < 0) {
    return nullptr;
  }
  // Replay may have truncated a torn final record.
  const std::optional<size_t> replayed_size = FileSize(fd);
  if (!replayed_size.has_value()) {
    return nullptr;
  }
  aof->current_size_ = *replayed_size;
  aof->base_size_ = *replayed_size;
  if (options.fsync == FsyncPolicy::kEverySecond) {
    // The project does not throw, but std::thread reports a failure to start
    // only by throwing std::system_error; catch it at this boundary so the
    // caller sees an ordinary error.
    try {
      aof->sync_thread_ =
          std::thread([instance = aof.get()] { instance->RunSyncThread(); });
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
      base_size_(file_size) {}

Aof::~Aof() {
  // Installs a rewrite whose child already finished, and stops one still
  // running, as Redis does on shutdown.
  CheckRewrite();
  if (rewrite_child_ > 0) {
    AbandonRewrite(AofError::kSnapshot);
  }
  // Redis syncs on shutdown whatever the policy.
  SyncNow();
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
  }
  sync_wakeup_.notify_all();
  if (sync_thread_.joinable()) {
    sync_thread_.join();
  }
  close(fd_);
}

bool Aof::Append(std::string_view command,
                 const std::vector<std::string_view>& args,
                 db::RedisDb* const db) {
  const size_t batch_size = batch_.size();
  if (!Encode(command, args, db, &batch_)) {
    // Drop any partially encoded record so the batch stays replayable.
    batch_.resize(batch_size);
    return false;
  }
  if (in_transaction_) {
    ++transaction_records_;
  }
  return true;
}

void Aof::BeginTransaction() {
  in_transaction_ = true;
  transaction_start_ = batch_.size();
  transaction_records_ = 0;
}

void Aof::EndTransaction() {
  if (!in_transaction_) {
    return;
  }
  in_transaction_ = false;
  if (transaction_records_ < 2) {
    return;
  }
  batch_.insert(transaction_start_, kMultiRecord);
  batch_.append(kExecRecord);
}

bool Aof::Flush() {
  LogSyncFailure();
  if (batch_.empty()) {
    const bool written = WriteUnwritten();
    return fsync_ != FsyncPolicy::kAlways || written;
  }
  // A flush inside EXEC (BGREWRITEAOF queued in MULTI) closes the records so
  // far as one atomic segment and starts another for the rest.
  const bool in_transaction = in_transaction_;
  if (in_transaction) {
    EndTransaction();
  }
  bool written = false;
  if (unwritten_.empty()) {
    int error_number = 0;
    const size_t size =
        WritePrefix(file_ops_.get(), fd_, batch_, &error_number);
    AddWritten(size);
    written = size == batch_.size();
    if (!written) {
      unwritten_.assign(batch_, size);
      FailWrite(error_number);
    }
  } else {
    unwritten_.append(batch_);
    written = WriteUnwritten();
  }
  if (rewrite_child_ > 0) {
    // The child's snapshot predates these mutations; the new file replays
    // them after it. Moving the batch costs an allocation only while a
    // rewrite runs.
    BufferForRewrite(std::move(batch_));
    batch_ = std::string();
  }
  batch_.clear();
  if (in_transaction) {
    BeginTransaction();
  }
  if (fsync_ != FsyncPolicy::kAlways) {
    return true;
  }
  return written && SyncWritten();
}

bool Aof::AcceptingWrites() const {
  return !write_failed_ && !sync_failed_.load(std::memory_order_relaxed);
}

std::string Aof::LastErrorText() const {
  const std::scoped_lock lock(mutex_);
  return last_errno_ != 0 ? std::strerror(last_errno_) : "unknown error";
}

bool Aof::SyncNow() {
  const bool written = Flush() && WriteUnwritten();
  return SyncWritten() && written;
}

bool Aof::WriteUnwritten() {
  if (unwritten_.empty()) {
    write_failed_ = false;
    return true;
  }
  int error_number = 0;
  const size_t size =
      WritePrefix(file_ops_.get(), fd_, unwritten_, &error_number);
  AddWritten(size);
  unwritten_.erase(0, size);
  if (!unwritten_.empty()) {
    FailWrite(error_number);
    return false;
  }
  if (write_failed_) {
    write_failed_ = false;
    RS_LOG_WARN("AOF writes recovered\n");
  }
  return true;
}

void Aof::AddWritten(size_t size) {
  if (size == 0) {
    return;
  }
  current_size_ += size;
  written_bytes_.fetch_add(size, std::memory_order_release);
}

void Aof::FailWrite(int error_number) {
  if (write_failed_) {
    return;
  }
  write_failed_ = true;
  RecordError(AofError::kWrite, error_number);
}

bool Aof::SyncWritten() {
  const std::scoped_lock lock(mutex_);
  if (sync_failed_.load(std::memory_order_relaxed)) {
    return false;
  }
  const uint64_t written = written_bytes_.load(std::memory_order_acquire);
  if (written == synced_bytes_) {
    return true;
  }
  if (!SyncFile(fd_)) {
    FailSyncLocked(errno);
    return false;
  }
  synced_bytes_ = written;
  return true;
}

void Aof::RunSyncThread() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    sync_wakeup_.wait_for(lock, kSyncInterval, [this] { return stopping_; });
    if (stopping_) {
      // The destructor syncs once the event-loop thread is done writing.
      return;
    }
    const uint64_t written = written_bytes_.load(std::memory_order_acquire);
    if (written == synced_bytes_ ||
        sync_failed_.load(std::memory_order_relaxed)) {
      continue;
    }
    // A duplicate keeps the file open if a rewrite replaces fd_ meanwhile;
    // syncing it syncs the same file.
    const int fd = dup(fd_);
    lock.unlock();
    const bool synced = fd >= 0 && SyncFile(fd);
    const int error_number = errno;
    if (fd >= 0) {
      close(fd);
    }
    lock.lock();
    if (!synced) {
      FailSyncLocked(error_number);
    } else if (written > synced_bytes_) {
      synced_bytes_ = written;
    }
  }
}

void Aof::FailSyncLocked(int error_number) {
  // No logging here: on the sync thread, formatting a message allocates, and
  // a fork at that moment could hand the child a held allocator lock. The
  // event-loop thread logs it instead (LogSyncFailure).
  sync_failed_.store(true, std::memory_order_relaxed);
  last_error_ = AofError::kSync;
  last_errno_ = error_number;
}

void Aof::LogSyncFailure() {
  if (sync_failure_logged_ || !sync_failed_.load(std::memory_order_relaxed)) {
    return;
  }
  sync_failure_logged_ = true;
  RS_LOG_ERROR("AOF fsync failed: %s; rejecting writes\n",
               LastErrorText().c_str());
}

void Aof::RecordError(AofError error, int error_number) {
  {
    const std::scoped_lock lock(mutex_);
    last_error_ = error;
    last_errno_ = error_number;
  }
  RS_LOG_ERROR("AOF %.*s failed: %s\n",
               static_cast<int>(ErrorName(error).size()),
               ErrorName(error).data(),
               error_number != 0 ? std::strerror(error_number) : "unknown");
}

RewriteResult Aof::StartRewrite(db::RedisDb* const db) {
  if (db == nullptr) {
    return RewriteResult::kError;
  }
  if (rewrite_child_ > 0) {
    return RewriteResult::kInProgress;
  }
  // Mutations already applied to `db` belong in the current file; the
  // snapshot covers them, so they must not land in the rewrite buffer.
  if (!Flush()) {
    return RewriteResult::kError;
  }
  last_rewrite_attempt_ = std::chrono::steady_clock::now();

  std::string path = path_ + ".rewrite.XXXXXX";
  const int rewrite_fd = mkstemp(path.data());
  if (rewrite_fd < 0) {
    rewrite_status_ = RewriteStatus::kFailed;
    RecordError(AofError::kWrite, errno);
    return RewriteResult::kError;
  }
  struct stat file_info{};
  const mode_t mode = fstat(fd_, &file_info) == 0
                          ? static_cast<mode_t>(file_info.st_mode & 0777)
                          : static_cast<mode_t>(0644);
  if (fcntl(rewrite_fd, F_SETFD, FD_CLOEXEC) < 0 ||
      fchmod(rewrite_fd, mode) < 0) {
    const int error_number = errno;
    close(rewrite_fd);
    unlink(path.c_str());
    rewrite_status_ = RewriteStatus::kFailed;
    RecordError(AofError::kWrite, error_number);
    return RewriteResult::kError;
  }

  if (before_fork_) {
    before_fork_();
  }
  const pid_t child = fork();
  if (child == 0) {
    RunRewriteChild(db, rewrite_fd);
  }
  if (after_fork_) {
    after_fork_();
  }
  if (child < 0) {
    const int error_number = errno;
    close(rewrite_fd);
    unlink(path.c_str());
    rewrite_status_ = RewriteStatus::kFailed;
    RecordError(AofError::kSnapshot, error_number);
    return RewriteResult::kError;
  }
  rewrite_child_ = child;
  rewrite_fd_ = rewrite_fd;
  rewrite_path_ = std::move(path);
  rewrite_buffer_.clear();
  rewrite_buffer_bytes_ = 0;
  return RewriteResult::kStarted;
}

void Aof::RunRewriteChild(db::RedisDb* const db, int rewrite_fd) const {
  // The child has only this thread. It takes no locks and does not log, since
  // another thread may have held one at the fork; allocating is safe because
  // the parent forks only while no other thread allocates.
  int status = kChildSucceeded;
  std::string block;
  auto sink = [this, rewrite_fd, &status, &block](std::string record) {
    if (block.empty()) {
      block = std::move(record);
    } else {
      block.append(record);
    }
    if (block.size() < limits_.snapshot_block_bytes) {
      return true;
    }
    std::array<std::string, 1> blocks{std::move(block)};
    block.clear();
    if (!WriteBlocks(file_ops_.get(), rewrite_fd, blocks)) {
      status = kChildWriteFailed;
      return false;
    }
    return true;
  };
  if (!BuildSnapshotRecords(db, limits_, &sink)) {
    if (status == kChildSucceeded) {
      status = kChildSnapshotFailed;
    }
  } else {
    std::array<std::string, 1> blocks{std::move(block)};
    if (!WriteBlocks(file_ops_.get(), rewrite_fd, blocks)) {
      status = kChildWriteFailed;
    } else if (!SyncFile(rewrite_fd)) {
      status = kChildSyncFailed;
    }
  }
  _exit(status);
}

void Aof::CheckRewrite() {
  if (rewrite_child_ <= 0) {
    return;
  }
  int wait_status = 0;
  const pid_t result = WaitForChild(rewrite_child_, &wait_status, WNOHANG);
  if (result != 0) {
    FinishRewrite(result < 0 ? -1 : wait_status);
  }
}

void Aof::WaitForRewrite() {
  if (rewrite_child_ <= 0) {
    return;
  }
  int wait_status = 0;
  const pid_t result = WaitForChild(rewrite_child_, &wait_status, 0);
  FinishRewrite(result < 0 ? -1 : wait_status);
}

void Aof::BufferForRewrite(std::string batch) {
  if (batch.size() > limits_.max_rewrite_buffer_bytes ||
      rewrite_buffer_bytes_ > limits_.max_rewrite_buffer_bytes - batch.size()) {
    AbandonRewrite(AofError::kRewriteBufferFull);
    return;
  }
  rewrite_buffer_bytes_ += batch.size();
  rewrite_buffer_.push_back(std::move(batch));
}

void Aof::FinishRewrite(int wait_status) {
  rewrite_child_ = -1;
  const bool child_succeeded = wait_status != -1 && WIFEXITED(wait_status) &&
                               WEXITSTATUS(wait_status) == kChildSucceeded;
  int error_number = 0;
  const AofError error =
      child_succeeded ? InstallRewrite(&error_number) : ChildError(wait_status);
  if (rewrite_fd_ >= 0) {
    close(rewrite_fd_);
    rewrite_fd_ = -1;
    unlink(rewrite_path_.c_str());
  }
  rewrite_buffer_.clear();
  rewrite_buffer_bytes_ = 0;
  if (error == AofError::kNone) {
    rewrite_status_ = RewriteStatus::kSucceeded;
    return;
  }
  rewrite_status_ = RewriteStatus::kFailed;
  RecordError(error, error_number);
}

AofError Aof::InstallRewrite(int* const error_number) {
  if (!WriteBlocks(file_ops_.get(), rewrite_fd_, rewrite_buffer_)) {
    *error_number = errno;
    return AofError::kWrite;
  }
  if (!SyncFile(rewrite_fd_)) {
    *error_number = errno;
    return AofError::kSync;
  }
  const std::optional<size_t> size = FileSize(rewrite_fd_);
  if (!size.has_value()) {
    *error_number = errno;
    return AofError::kWrite;
  }
  if (file_ops_->Rename(rewrite_path_.c_str(), path_.c_str()) < 0) {
    *error_number = errno;
    return AofError::kRename;
  }
  int old_fd = -1;
  {
    // The new file holds every mutation and is synced, so it also clears a
    // failed write or sync on the old one.
    const std::scoped_lock lock(mutex_);
    old_fd = fd_;
    fd_ = rewrite_fd_;
    synced_bytes_ = written_bytes_.load(std::memory_order_acquire);
    sync_failed_.store(false, std::memory_order_relaxed);
  }
  sync_failure_logged_ = false;
  rewrite_fd_ = -1;
  close(old_fd);
  unwritten_.clear();
  write_failed_ = false;
  current_size_ = *size;
  base_size_ = *size;
  if (!SyncParentDirectory()) {
    *error_number = errno;
    const std::scoped_lock lock(mutex_);
    FailSyncLocked(*error_number);
    return AofError::kDirectorySync;
  }
  return AofError::kNone;
}

void Aof::AbandonRewrite(AofError error) {
  kill(rewrite_child_, SIGKILL);
  int wait_status = 0;
  WaitForChild(rewrite_child_, &wait_status, 0);
  rewrite_child_ = -1;
  close(rewrite_fd_);
  rewrite_fd_ = -1;
  unlink(rewrite_path_.c_str());
  rewrite_buffer_.clear();
  rewrite_buffer_bytes_ = 0;
  rewrite_status_ = RewriteStatus::kFailed;
  RecordError(error, 0);
}

bool Aof::ShouldAutoRewrite() const {
  const size_t current_size = current_size_ + batch_.size();
  if (rewrite_child_ > 0 || auto_rewrite_percentage_ == 0 ||
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
  // Compare growth / base_size_ with the percentage exactly, without overflow.
  constexpr uint64_t kPercent = 100;
  return WideMultiply(growth, kPercent) >=
         WideMultiply(base_size_, auto_rewrite_percentage_);
}

AofState Aof::State() const {
  const std::scoped_lock lock(mutex_);
  const size_t pending = batch_.size() + unwritten_.size();
  return {!write_failed_ && !sync_failed_.load(std::memory_order_relaxed),
          rewrite_child_ > 0,
          rewrite_status_,
          last_error_,
          current_size_ + pending,
          base_size_,
          pending};
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
    // Records between MULTI and EXEC are copied out of `input` and applied
    // together at EXEC, so a transaction cut off by a crash is not applied.
    std::optional<size_t> transaction_start;
    ReplayTransaction transaction;

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
        const request_parser::ParseResult parsed =
            request_parser::Parse(remaining, &name, &args);
        if (parsed.status == request_parser::ParseStatus::kIncomplete) {
          break;
        }
        if (parsed.status != request_parser::ParseStatus::kComplete) {
          return false;
        }
        if (parsed.consumed > limits_.max_replay_command_bytes) {
          return false;
        }
        if (name == "MULTI" && args.empty()) {
          if (transaction_start.has_value()) {
            return false;
          }
          transaction_start = complete_bytes;
        } else if (name == "EXEC" && args.empty()) {
          const auto execute = [&client](const command::Command* command,
                                         command::CommandArgs* views) {
            return client.ExecuteForReplay(command, views);
          };
          if (!transaction_start.has_value() || !transaction.Apply(execute)) {
            return false;
          }
          transaction_start.reset();
        } else {
          const command::Command* metadata = command::Find(name);
          if (metadata == nullptr ||
              metadata->access != command::CommandAccess::kWrite ||
              !metadata->arity.Accepts(args.size())) {
            return false;
          }
          if (transaction_start.has_value()) {
            transaction.Queue(metadata, args);
          } else if (!client.ExecuteForReplay(metadata, &args)) {
            return false;
          }
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

    const bool truncated = !input.Empty() || transaction_start.has_value();
    if (truncated) {
      const size_t valid_bytes = transaction_start.value_or(complete_bytes);
      RS_LOG_DEBUG("truncating incomplete AOF tail at %zu bytes\n",
                   valid_bytes);
      if (valid_bytes >
              static_cast<uintmax_t>(std::numeric_limits<off_t>::max()) ||
          ftruncate(fd_, static_cast<off_t>(valid_bytes)) < 0) {
        return false;
      }
    }
    return !truncated || SyncFile(fd_);
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
    const auto [object, expire] = db->LookupKeyWithExpiration(args[0]);
    if (object == nullptr) {
      return AppendSingleKeyCommand("DEL", args[0], output);
    }
    if (object->Type() != db::RedisObject::ObjectType::kString) {
      return false;
    }
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
    const std::optional<int64_t> expire = db->Expiration(args[0]);
    if (expire.has_value()) {
      return AppendExpireAt(args[0], *expire, output);
    }
    return AppendSingleKeyCommand("DEL", args[0], output);
  }
  return AppendCommand(command, args, output);
}

}  // namespace redis_simple::aof
