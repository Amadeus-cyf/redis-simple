#include "server/aof.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
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
#include "utils/check.h"
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

// Multi-part AOF names, as in Redis 7.
constexpr std::string_view kBaseSuffix = ".base.aof";
constexpr std::string_view kIncrementSuffix = ".incr.aof";
constexpr std::string_view kManifestSuffix = ".manifest";
constexpr std::string_view kTempPrefix = "temp-";

// Auto-rewrite backoff after repeated failures, as in Redis: from the third
// consecutive failure, wait a minute, doubling up to an hour.
constexpr size_t kFailuresBeforeBackoff = 3;
constexpr std::chrono::minutes kFirstBackoff{1};
constexpr std::chrono::minutes kMaxBackoff{60};

char TypeCode(ManifestFile::Type type) {
  switch (type) {
    case ManifestFile::Type::kBase:
      return 'b';
    case ManifestFile::Type::kIncrement:
      return 'i';
    case ManifestFile::Type::kHistory:
      return 'h';
  }
  utils::Unreachable();
}

std::optional<ManifestFile::Type> ParseType(std::string_view code) {
  if (code == "b") {
    return ManifestFile::Type::kBase;
  }
  if (code == "i") {
    return ManifestFile::Type::kIncrement;
  }
  if (code == "h") {
    return ManifestFile::Type::kHistory;
  }
  return std::nullopt;
}

// A name the manifest can hold: not empty, no directory, and no whitespace,
// which separates the manifest's fields.
bool ValidFileName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         std::ranges::none_of(name, [](char c) {
           return c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
         });
}

std::optional<std::string> ReadWholeFile(const std::string& path) {
  int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(path.c_str(), open_flags);
  if (fd < 0) {
    return std::nullopt;
  }
  std::string contents;
  std::array<char, 4096> chunk{};
  while (true) {
    const ssize_t size = read(fd, chunk.data(), chunk.size());
    if (size > 0) {
      contents.append(chunk.data(), static_cast<size_t>(size));
      continue;
    }
    if (size < 0 && errno == EINTR) {
      continue;
    }
    close(fd);
    if (size < 0) {
      return std::nullopt;
    }
    return contents;
  }
}

bool PathExists(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0;
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
    FailSyncLocked(errno, fd_sequence_);
    return false;
  }
  synced_bytes_ = written;
  return true;
}

void Aof::RunSyncThread() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    sync_wakeup_.wait_for(lock, kSyncInterval, [this] {
      return stopping_ || !retired_files_.empty();
    });
    // Files a rewrite replaced: sync their last writes, then close them.
    while (!retired_files_.empty()) {
      const RetiredFile retired = retired_files_.back();
      retired_files_.pop_back();
      lock.unlock();
      const bool synced = SyncFile(retired.fd);
      const int error_number = errno;
      close(retired.fd);
      lock.lock();
      if (!synced) {
        FailSyncLocked(error_number, retired.sequence);
      } else if (retired.written_mark > synced_bytes_) {
        synced_bytes_ = retired.written_mark;
      }
    }
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
    const uint64_t sequence = fd_sequence_;
    lock.unlock();
    const bool synced = fd >= 0 && SyncFile(fd);
    const int error_number = errno;
    if (fd >= 0) {
      close(fd);
    }
    lock.lock();
    if (!synced) {
      FailSyncLocked(error_number, sequence);
    } else if (written > synced_bytes_) {
      synced_bytes_ = written;
    }
  }
}

void Aof::FailSyncLocked(int error_number, uint64_t sequence) {
  // No logging here: on the sync thread, formatting a message allocates, and
  // a fork at that moment could hand the child a held allocator lock. The
  // event-loop thread logs it instead (LogSyncFailure).
  if (!sync_failed_.load(std::memory_order_relaxed) ||
      sequence > sync_failed_sequence_) {
    sync_failed_sequence_ = sequence;
  }
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

bool Aof::SyncFile(int fd) const { return aof::SyncFile(file_ops_.get(), fd); }

bool Aof::SyncParentDirectory() const {
  // Renames of the manifest and base files happen inside the directory.
  int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
  open_flags |= O_DIRECTORY;
#endif
  const int directory_fd = open(directory_.c_str(), open_flags);
  if (directory_fd < 0) {
    return false;
  }
  const bool synced = SyncFile(directory_fd);
  const bool closed = close(directory_fd) == 0;
  return synced && closed;
}

std::string Manifest::Encode() const {
  std::string text;
  const auto append = [&text](const ManifestFile& file) {
    text.append("file ")
        .append(file.name)
        .append(" seq ")
        .append(std::to_string(file.sequence))
        .append(" type ")
        .push_back(TypeCode(file.type));
    text.push_back('\n');
  };
  if (base.has_value()) {
    append(*base);
  }
  for (const ManifestFile& file : history) {
    append(file);
  }
  for (const ManifestFile& file : increments) {
    append(file);
  }
  return text;
}

std::optional<Manifest> Manifest::Parse(std::string_view text) {
  Manifest manifest;
  while (!text.empty()) {
    const size_t line_end = text.find('\n');
    std::string_view line = text.substr(0, line_end);
    text = line_end == std::string_view::npos ? std::string_view()
                                              : text.substr(line_end + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    // Space-separated key/value pairs, in any order.
    ManifestFile file;
    bool has_name = false;
    bool has_sequence = false;
    bool has_type = false;
    while (!line.empty()) {
      const size_t key_end = line.find(' ');
      if (key_end == std::string_view::npos) {
        return std::nullopt;
      }
      const std::string_view key = line.substr(0, key_end);
      line.remove_prefix(key_end + 1);
      const size_t value_end = line.find(' ');
      const std::string_view value = line.substr(0, value_end);
      line = value_end == std::string_view::npos ? std::string_view()
                                                 : line.substr(value_end + 1);
      if (key == "file") {
        if (!ValidFileName(value)) {
          return std::nullopt;
        }
        file.name = value;
        has_name = true;
      } else if (key == "seq") {
        const auto [end, error] = std::from_chars(
            value.data(), value.data() + value.size(), file.sequence);
        if (error != std::errc() || end != value.data() + value.size()) {
          return std::nullopt;
        }
        has_sequence = true;
      } else if (key == "type") {
        const std::optional<ManifestFile::Type> type = ParseType(value);
        if (!type.has_value()) {
          return std::nullopt;
        }
        file.type = *type;
        has_type = true;
      }
      // Unknown keys are ignored, so newer manifests stay loadable.
    }
    if (!has_name || !has_sequence || !has_type) {
      return std::nullopt;
    }
    switch (file.type) {
      case ManifestFile::Type::kBase:
        if (manifest.base.has_value()) {
          return std::nullopt;
        }
        manifest.base_sequence =
            std::max(manifest.base_sequence, file.sequence);
        manifest.base = std::move(file);
        break;
      case ManifestFile::Type::kIncrement:
        // Incremental files replay in order.
        if (!manifest.increments.empty() &&
            file.sequence <= manifest.increments.back().sequence) {
          return std::nullopt;
        }
        manifest.increment_sequence =
            std::max(manifest.increment_sequence, file.sequence);
        manifest.increments.push_back(std::move(file));
        break;
      case ManifestFile::Type::kHistory:
        // Keep sequence numbers unique across files not yet deleted.
        if (file.name.ends_with(kBaseSuffix)) {
          manifest.base_sequence =
              std::max(manifest.base_sequence, file.sequence);
        } else if (file.name.ends_with(kIncrementSuffix)) {
          manifest.increment_sequence =
              std::max(manifest.increment_sequence, file.sequence);
        }
        manifest.history.push_back(std::move(file));
        break;
    }
  }
  return manifest;
}

std::unique_ptr<Aof> Aof::Open(const Options& options, db::RedisDb* const db) {
  if (db == nullptr || options.directory.empty() ||
      !ValidFileName(options.file_name) ||
      options.limits.max_replay_command_bytes == 0 ||
      options.limits.snapshot_block_bytes == 0) {
    return nullptr;
  }
  if (mkdir(options.directory.c_str(), 0755) < 0 && errno != EEXIST) {
    return nullptr;
  }
  auto aof = std::unique_ptr<Aof>(new Aof(options, options.directory));
  if (!aof->LoadManifest() || !aof->Replay(db) || !aof->OpenLastIncrement()) {
    return nullptr;
  }
  aof->DeleteHistory();
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

Aof::Aof(const Options& options, std::string directory)
    : directory_(std::move(directory)),
      file_name_(options.file_name),
      fsync_(options.fsync),
      auto_rewrite_min_bytes_(options.auto_rewrite_min_bytes),
      auto_rewrite_percentage_(options.auto_rewrite_percentage),
      limits_(options.limits),
      file_ops_(options.file_ops != nullptr ? options.file_ops
                                            : DefaultFileOps()) {}

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
  // Files replaced since the sync thread last ran.
  for (const RetiredFile& retired : retired_files_) {
    SyncFile(retired.fd);
    close(retired.fd);
  }
  if (fd_ >= 0) {
    close(fd_);
  }
}

std::string Aof::FilePath(std::string_view name) const {
  std::string path = directory_;
  path.push_back('/');
  path.append(name);
  return path;
}

std::string Aof::ManifestPath() const {
  return FilePath(file_name_ + std::string(kManifestSuffix));
}

bool Aof::LoadManifest() {
  if (PathExists(ManifestPath())) {
    const std::optional<std::string> text = ReadWholeFile(ManifestPath());
    if (!text.has_value()) {
      return false;
    }
    std::optional<Manifest> manifest = Manifest::Parse(*text);
    if (!manifest.has_value()) {
      RS_LOG_ERROR("malformed AOF manifest %s\n", ManifestPath().c_str());
      return false;
    }
    manifest_ = std::move(*manifest);
    return true;
  }
  // Upgrade a single-file AOF, as Redis 7 does: move it into the directory
  // as the base file, keeping its name.
  if (PathExists(file_name_)) {
    if (rename(file_name_.c_str(), FilePath(file_name_).c_str()) < 0) {
      return false;
    }
    Manifest manifest;
    manifest.base = ManifestFile{file_name_, 1, ManifestFile::Type::kBase};
    manifest.base_sequence = 1;
    if (PersistManifest(manifest) != AofError::kNone) {
      return false;
    }
    manifest_ = std::move(manifest);
    RS_LOG_WARN("moved %s into %s as the AOF base file\n", file_name_.c_str(),
                directory_.c_str());
  }
  return true;
}

bool Aof::Replay(db::RedisDb* const db) {
  std::vector<const ManifestFile*> files;
  if (manifest_.base.has_value()) {
    files.push_back(&*manifest_.base);
  }
  for (const ManifestFile& file : manifest_.increments) {
    files.push_back(&file);
  }
  size_t total_size = 0;
  for (size_t index = 0; index < files.size(); ++index) {
    int open_flags = O_RDWR;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    const std::string path = FilePath(files[index]->name);
    const int fd = open(path.c_str(), open_flags);
    if (fd < 0) {
      // A listed incremental file that was never created holds nothing.
      if (errno == ENOENT &&
          files[index]->type == ManifestFile::Type::kIncrement) {
        continue;
      }
      RS_LOG_ERROR("cannot open AOF file %s\n", path.c_str());
      return false;
    }
    // Only the last file may end in a torn record, as in Redis.
    const bool replayed = ReplayFile(db, fd, index + 1 == files.size());
    const std::optional<size_t> size = FileSize(fd);
    close(fd);
    if (!replayed || !size.has_value()) {
      RS_LOG_ERROR("cannot load AOF file %s\n", path.c_str());
      return false;
    }
    if (files[index]->type == ManifestFile::Type::kBase) {
      base_size_ = *size;
    }
    total_size += *size;
  }
  current_size_ = total_size;
  return true;
}

bool Aof::OpenLastIncrement() {
  // Appends continue in the last incremental file, as in Redis; only a
  // rewrite starts a new one.
  if (manifest_.increments.empty()) {
    return OpenNextIncrement();
  }
  const ManifestFile& last = manifest_.increments.back();
  int open_flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(FilePath(last.name).c_str(), open_flags, 0644);
  if (fd < 0 || lseek(fd, 0, SEEK_END) < 0) {
    if (fd >= 0) {
      close(fd);
    }
    return false;
  }
  SwitchFile(fd, last.sequence);
  return true;
}

bool Aof::OpenNextIncrement() {
  Manifest manifest = manifest_;
  const uint64_t sequence = manifest.increment_sequence + 1;
  const std::string name = file_name_ + "." + std::to_string(sequence) +
                           std::string(kIncrementSuffix);
  int open_flags = O_RDWR | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(FilePath(name).c_str(), open_flags, 0644);
  if (fd < 0) {
    RecordError(AofError::kWrite, errno);
    return false;
  }
  manifest.increments.push_back(
      ManifestFile{name, sequence, ManifestFile::Type::kIncrement});
  manifest.increment_sequence = sequence;
  const AofError error = PersistManifest(manifest);
  const int error_number = errno;
  if (error != AofError::kNone && error != AofError::kDirectorySync) {
    close(fd);
    unlink(FilePath(name).c_str());
    RecordError(error, error_number);
    return false;
  }
  // After the rename the new manifest is live, durable or not.
  manifest_ = std::move(manifest);
  SwitchFile(fd, sequence);
  if (error == AofError::kDirectorySync) {
    FailDirectorySync(error_number);
  }
  return true;
}

void Aof::FailDirectorySync(int error_number) {
  // A rename that may not survive a crash: reject writes until a later
  // rewrite persists a manifest safely.
  {
    const std::scoped_lock lock(mutex_);
    FailSyncLocked(error_number, fd_sequence_);
    last_error_ = AofError::kDirectorySync;
  }
  history_unsafe_to_delete_ = true;
}

void Aof::SwitchFile(int fd, uint64_t sequence) {
  int old_fd = -1;
  {
    const std::scoped_lock lock(mutex_);
    old_fd = fd_;
    if (old_fd >= 0 && fsync_ == FsyncPolicy::kEverySecond) {
      // The sync thread syncs the replaced file's last writes, then closes
      // it, so the switch does not wait for the disk.
      retired_files_.push_back(
          {old_fd, fd_sequence_,
           written_bytes_.load(std::memory_order_acquire)});
      old_fd = -1;
    }
    fd_ = fd;
    fd_sequence_ = sequence;
  }
  sync_wakeup_.notify_all();
  if (old_fd >= 0) {
    // `always` synced every write already; `no` leaves it to the system.
    close(old_fd);
  }
}

AofError Aof::PersistManifest(const Manifest& manifest) {
  const std::string temp_path = FilePath(std::string(kTempPrefix) + file_name_ +
                                         std::string(kManifestSuffix));
  int open_flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
  open_flags |= O_CLOEXEC;
#endif
  const int fd = open(temp_path.c_str(), open_flags, 0644);
  if (fd < 0) {
    return AofError::kWrite;
  }
  const std::string text = manifest.Encode();
  int error_number = 0;
  const bool written =
      WritePrefix(file_ops_.get(), fd, text, &error_number) == text.size();
  const bool synced = written && SyncFile(fd);
  close(fd);
  if (!written || !synced) {
    unlink(temp_path.c_str());
    errno = error_number != 0 ? error_number : errno;
    return written ? AofError::kSync : AofError::kWrite;
  }
  if (file_ops_->Rename(temp_path.c_str(), ManifestPath().c_str()) < 0) {
    const int rename_errno = errno;
    unlink(temp_path.c_str());
    errno = rename_errno;
    return AofError::kRename;
  }
  return SyncParentDirectory() ? AofError::kNone : AofError::kDirectorySync;
}

void Aof::DeleteHistory() {
  // After a manifest whose rename may not be durable, a crash could bring
  // back the previous manifest, which still needs these files.
  if (manifest_.history.empty() || history_unsafe_to_delete_) {
    return;
  }
  // The manifest no longer lists them as live, so deleting them first is
  // safe; a crash before the second persist only leaves them listed.
  for (const ManifestFile& file : manifest_.history) {
    unlink(FilePath(file.name).c_str());
  }
  Manifest manifest = manifest_;
  manifest.history.clear();
  if (PersistManifest(manifest) == AofError::kNone) {
    manifest_ = std::move(manifest);
  }
}

RewriteResult Aof::StartRewrite(db::RedisDb* const db) {
  if (db == nullptr) {
    return RewriteResult::kError;
  }
  if (rewrite_child_ > 0) {
    return RewriteResult::kInProgress;
  }
  // Every mutation applied to `db` must be in the current incremental file
  // before the switch: the snapshot covers them, and the next file must not.
  // Bytes stuck after a write error would land in both, so wait them out.
  if (!Flush() || !unwritten_.empty()) {
    return RewriteResult::kError;
  }
  last_rewrite_attempt_ = std::chrono::steady_clock::now();
  if (!OpenNextIncrement()) {
    FailRewrite();
    return RewriteResult::kError;
  }

  std::string path = FilePath(std::string(kTempPrefix) + "rewriteaof-XXXXXX");
  const int rewrite_fd = mkstemp(path.data());
  if (rewrite_fd < 0) {
    RecordError(AofError::kWrite, errno);
    FailRewrite();
    return RewriteResult::kError;
  }
  if (fcntl(rewrite_fd, F_SETFD, FD_CLOEXEC) < 0 ||
      fchmod(rewrite_fd, 0644) < 0) {
    const int error_number = errno;
    close(rewrite_fd);
    unlink(path.c_str());
    RecordError(AofError::kWrite, error_number);
    FailRewrite();
    return RewriteResult::kError;
  }

  if (before_fork_) {
    before_fork_();
  }
  const pid_t child = fork();
  if (child == 0) {
    RunRewriteChild(db, rewrite_fd);
  }
  const int fork_errno = errno;
  if (after_fork_) {
    after_fork_();
  }
  if (child < 0) {
    close(rewrite_fd);
    unlink(path.c_str());
    RecordError(AofError::kSnapshot, fork_errno);
    FailRewrite();
    return RewriteResult::kError;
  }
  rewrite_child_ = child;
  rewrite_fd_ = rewrite_fd;
  rewrite_path_ = std::move(path);
  rewrite_increment_sequence_ = manifest_.increment_sequence;
  return RewriteResult::kStarted;
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
  }
  if (error == AofError::kNone) {
    rewrite_status_ = RewriteStatus::kSucceeded;
    consecutive_rewrite_failures_ = 0;
    DeleteHistory();
    return;
  }
  unlink(rewrite_path_.c_str());
  RecordError(error, error_number);
  FailRewrite();
}

AofError Aof::InstallRewrite(int* const error_number) {
  // The child synced the snapshot; it becomes the next base file.
  const std::optional<size_t> base_size = FileSize(rewrite_fd_);
  if (!base_size.has_value()) {
    *error_number = errno;
    return AofError::kWrite;
  }
  Manifest manifest = manifest_;
  const uint64_t base_sequence = manifest.base_sequence + 1;
  const std::string base_name = file_name_ + "." +
                                std::to_string(base_sequence) +
                                std::string(kBaseSuffix);
  if (file_ops_->Rename(rewrite_path_.c_str(), FilePath(base_name).c_str()) <
      0) {
    *error_number = errno;
    return AofError::kRename;
  }
  // The old base and the incremental files before the fork are history.
  if (manifest.base.has_value()) {
    manifest.base->type = ManifestFile::Type::kHistory;
    manifest.history.push_back(std::move(*manifest.base));
  }
  const auto first_kept = std::ranges::find_if(
      manifest.increments, [this](const ManifestFile& file) {
        return file.sequence >= rewrite_increment_sequence_;
      });
  for (auto file = manifest.increments.begin(); file != first_kept; ++file) {
    file->type = ManifestFile::Type::kHistory;
    manifest.history.push_back(std::move(*file));
  }
  manifest.increments.erase(manifest.increments.begin(), first_kept);
  manifest.base =
      ManifestFile{base_name, base_sequence, ManifestFile::Type::kBase};
  manifest.base_sequence = base_sequence;
  const AofError persist_error = PersistManifest(manifest);
  if (persist_error != AofError::kNone &&
      persist_error != AofError::kDirectorySync) {
    *error_number = errno;
    // The old manifest still describes the AOF; drop the unused base.
    unlink(FilePath(base_name).c_str());
    return persist_error;
  }
  // After the rename the new manifest is live, durable or not.
  manifest_ = std::move(manifest);
  if (persist_error == AofError::kDirectorySync) {
    *error_number = errno;
    FailDirectorySync(*error_number);
  }

  size_t increment_size = 0;
  for (const ManifestFile& file : manifest_.increments) {
    struct stat info {};
    if (stat(FilePath(file.name).c_str(), &info) == 0 && info.st_size > 0) {
      increment_size += static_cast<size_t>(info.st_size);
    }
  }
  base_size_ = *base_size;
  current_size_ = *base_size + increment_size;
  if (persist_error == AofError::kDirectorySync) {
    return persist_error;
  }
  history_unsafe_to_delete_ = false;
  {
    // A sync that failed on a file the new base replaces no longer matters.
    const std::scoped_lock lock(mutex_);
    if (sync_failed_.load(std::memory_order_relaxed) &&
        sync_failed_sequence_ < rewrite_increment_sequence_) {
      sync_failed_.store(false, std::memory_order_relaxed);
      sync_failure_logged_ = false;
    }
  }
  return AofError::kNone;
}

void Aof::FailRewrite() {
  rewrite_status_ = RewriteStatus::kFailed;
  ++consecutive_rewrite_failures_;
}

void Aof::AbandonRewrite(AofError error) {
  kill(rewrite_child_, SIGKILL);
  int wait_status = 0;
  WaitForChild(rewrite_child_, &wait_status, 0);
  rewrite_child_ = -1;
  close(rewrite_fd_);
  rewrite_fd_ = -1;
  unlink(rewrite_path_.c_str());
  RecordError(error, 0);
  FailRewrite();
}

bool Aof::ShouldAutoRewrite() const {
  const size_t current_size = current_size_ + batch_.size();
  if (rewrite_child_ > 0 || auto_rewrite_percentage_ == 0 ||
      current_size == 0 || current_size < auto_rewrite_min_bytes_) {
    return false;
  }
  if (consecutive_rewrite_failures_ >= kFailuresBeforeBackoff) {
    // A failing rewrite leaves an incremental file behind each time, so back
    // off: one minute, doubling, up to an hour.
    const size_t doublings = std::min<size_t>(
        consecutive_rewrite_failures_ - kFailuresBeforeBackoff, 6);
    const std::chrono::minutes delay =
        std::min(kFirstBackoff * (int64_t{1} << static_cast<int>(doublings)),
                 kMaxBackoff);
    if (std::chrono::steady_clock::now() - last_rewrite_attempt_ < delay) {
      return false;
    }
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
          pending,
          consecutive_rewrite_failures_};
}

bool Aof::ReplayFile(db::RedisDb* const db, int fd, bool last) {
  db->SetLoading(true);
  const bool replayed = [this, db, fd, last] {
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
        bytes_read = read(fd, chunk.data(), chunk.size());
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
    if (truncated && !last) {
      return false;
    }
    if (truncated) {
      const size_t valid_bytes = transaction_start.value_or(complete_bytes);
      RS_LOG_DEBUG("truncating incomplete AOF tail at %zu bytes\n",
                   valid_bytes);
      if (valid_bytes >
              static_cast<uintmax_t>(std::numeric_limits<off_t>::max()) ||
          ftruncate(fd, static_cast<off_t>(valid_bytes)) < 0) {
        return false;
      }
    }
    return !truncated || SyncFile(fd);
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
